# 设计：构建期内存统计 + 两门控下刷（segment flush + 内部分段 spill）+ k-way merge

Status: draft（待评审）
Owner: SNII writer
Scope: `core/include/snii/writer/spimi_term_buffer.{h,cpp}`、`compact_posting_pool.h`、
`spillable_byte_buffer.h`、`logical_index_writer.{h,cpp}`、`SniiIndexInput`，
新增 `core/include/snii/writer/memory_reporter.h`；与 Doris memory 集成边界。

> **本设计历经两次修正：**
> 1. 取代「共享内存预算器（victim 选择 / charge / 预算器自做 spill）」——那一版把下刷决策放进 SNII，
>    招致 victim 误 spill、计费双减、锁内 spill 等 blocker。
> 2. 取代「单一外部 flush()（删内部阈值）」——对抗评审后查证，真实模型是**两个独立门控**，
>    内部阈值不能删（它本身就是门控 2）。本设计按真实 Doris 行为重写。

---

## 0. 两门控模型（Why）

下刷由**两个相互独立的门控**驱动，行为与触发都不同：

| | **门控 1：Doris 内存压力 → 整段 flush** | **门控 2：倒排自身 buffer 上限 → 内部分段 spill** |
|---|---|---|
| 触发者 | Doris `MemTableMemoryLimiter`（软/硬限） | SNII 自己（本倒排索引内存 > 配置上限，如 512 MiB） |
| 触发线程 | Doris flush 线程（memtable 已封口） | 累积线程内**自触发**（导入线程） |
| 行为 | SNII `finalize()` → **一个完整 segment `.idx`** | spill 一个**小段到临时目录** + 继续累积 |
| 与「分段」关系 | **无关**（就是把整段提前 flush 出去） | **这就是内部分段** |
| 合并 | —— | segment flush 时 **k-way merge** 所有小段 |
| SNII 现状 | `finalize()` 已产出 `.idx` | **已实现**（`spill_to_run` + `MergeRuns`） |

```
导入中（行进入，倒排在内存累积）
   │
   ├─[门控 2：本倒排 > 512M] SNII 自触发 → spill 小段到临时目录 + 继续    ← 可多次（内部分段）
   │
   └─[门控 1：Doris 全局/load 内存压力] MemTableMemoryLimiter 挑该 memtable flush
         → segment 写出 → 倒排 writer finish()（既有路径）→ SNII finalize
         → 把内存残留 + 临时小段 k-way merge 成完整 .idx
```

**SNII 要做的，是把内存统计准确报给 Doris（让门控 1 看得见），并保留/接好内部分段（门控 2）。**
下刷「何时整段 flush」由 Doris 决定；「本倒排是否内部分段」由 SNII 自己的 buffer 上限决定。

**非目标（明确删除）**：SNII **不**实现 victim 选择、共享预算器、charge/relieve、跨 buffer 协调；
**也不**靠「Doris 跨线程调一个 flush() 去动正在累积的 buffer」——门控 1 是 finalize（封口后），
门控 2 是累积线程内自触发，二者都无跨线程动 buffer 的竞争。

---

## 1. 现状对照：素材几乎全有

| 能力 | 现状 | 出处 |
|---|---|---|
| 各模块自报字节 | **已有**：`CompactPostingPool::arena_bytes()`、`SpillableByteBuffer::size()`、`slot_of_` 容量 | `compact_posting_pool.h:105`、`spillable_byte_buffer.h:40` |
| 门控 2 的 spill 内核（可重入） | **已有**：`spill_to_run()`（落小段、reset arena、继续） | `spimi_term_buffer.cpp:451` |
| 门控 2 的内部阈值触发 | **已有**：`live_bytes_ >= spill_threshold_bytes_`（**但量报错，见 §2.2**） | `spimi_term_buffer.cpp:159` |
| segment flush 的 k-way merge | **已有**：`MergeRuns()` | `spill_run_codec.h:177` |
| 门控 1 的整段产出 | **已有**：`finalize()` 产出 `.idx` | `snii_compound_writer.cpp` |
| **内存上报给 Doris（门控 1 可见）** | **缺** | 本设计 §2 |
| **阈值接 Doris 配置（门控 2 = 512M）** | **缺**（现为本地常量） | 本设计 §3 |

净新增只有：**(1) 内存准确上报 Doris**、**(2) 门控 2 阈值接配置并改用真实字节**。下刷内核与 merge 复用现有。

---

## 2. 准确内存统计（writer 级）

### 2.1 `MemoryReporter`：极简累加器 + Doris 桥接

```cpp
// core/include/snii/writer/memory_reporter.h
namespace snii::writer {

// Per-WRITER accurate byte counter for build-time RAM (one per SniiCompoundWriter =
// one per segment's inverted index). Modules report their own resident-byte deltas;
// current_bytes() is that writer's accurate live usage. OBSERVE-ONLY -- SNII never
// makes a flush decision from it (gate 1 belongs to Doris; gate 2 is the internal
// threshold). consume_release mirrors the delta into Doris's LOAD MemTracker so the
// inverted-index RAM is counted by MemTableMemoryLimiter's pressure decision.
class MemoryReporter {
 public:
  using ConsumeReleaseFn = std::function<void(int64_t delta)>;  // null off-Doris
  explicit MemoryReporter(ConsumeReleaseFn consume_release = nullptr)
      : consume_release_(std::move(consume_release)) {}

  void report(int64_t delta) {                       // >0 grow, <0 shrink/free
    current_.fetch_add(delta, std::memory_order_relaxed);
    if (consume_release_) consume_release_(delta);   // mirror into Doris load tracker
  }
  int64_t current_bytes() const { return current_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> current_{0};
  ConsumeReleaseFn consume_release_;
};

}  // namespace snii::writer
```

无锁、无 registry、无 victim、无生命周期悬垂——只是个计数器 + 一个可选桥接回调。

### 2.2 报真实字节（修正：`arena_bytes()`，不是 `live_bytes_`）

**对抗评审实测的真 bug**：`account_token`（`live_bytes_` 的唯一写入处）被
`spill_threshold_bytes_ != 0` 门控（`spimi_term_buffer.cpp:150`）。所以 `live_bytes_` 是个
**仅在设了阈值时才更新的估计值**，且与 `arena_bytes()`（真实 arena）**口径不同**——若两者都报会重复计。

**纪律**：
- 倒排 posting 内存的**单一真值 = `CompactPostingPool::arena_bytes()`**（恒更新、含 block slack、
  本就是 4 GiB 兜底检查用的真实量，`:158`）。**报它，不报 `live_bytes_`。**
- 加上 `slot_of_.capacity() * 4`（vocab×4B 的 slot 索引，**跨 spill 驻留**、`spill_to_run` 不释放，
  评审指出这块若漏报会让 Doris 低估真实 RSS）。
- dict 区报 `SpillableByteBuffer::size()`（`ram_bytes_`）。
- 每处增减**恰好报一次**（增正、减负）：arena 增长 / `pool_.reset()`（spill 归还）/ `merge_runs`
  释放 `slot_of_` / dict spill 转盘——每一处都是必报点。

### 2.3 与 Doris 的对接（门控 1 的可见性）

- 粒度 **writer 级**：每个 `SniiCompoundWriter`（= 一个 segment 倒排）一个 `MemoryReporter`，
  其内各列模块都报到它。
- `consume_release_` 由合并侧注入，把该 writer 的 delta **consume/release 进 Doris 的 LOAD 内存
  tracker**（倒排 RAM 计入 load 内存）。于是 `MemTableMemoryLimiter` 的软/硬限判定**看得见倒排内存**，
  压力大时挑这个 memtable flush（→ 门控 1）。
- 未连 Doris（bench / 单测）时回调为空，只走本地 atomic，`current_bytes()` 仍可读。

---

## 3. 两门控的接入

### 3.1 门控 1：Doris 内存压力 → memtable flush → 倒排 writer finish → SNII 落 .idx

接线点是 **Doris 的倒排 writer 接口，不是内存限制器**。SNII 坐在 Doris
`InvertedIndexColumnWriter`（`init` / `add_values` / `add_nulls` / `finish`，
`inverted_index_writer.h:74-107`）背后：`add_values` 把 token 喂给 SNII 的 `SpimiTermBuffer`，
`finish()` 调 SNII 落出完整 `.idx`。完整链路：

```
MemTableMemoryLimiter（软/硬限，memtable_memory_limiter.cpp:124）挑某 memtable flush
  → memtable flush → segment 写出（Doris 既有流程）
    → InvertedIndexColumnWriter::finish()（既有调用点）
      → SNII finish/finalize → 完整 .idx（含把内存残留 + 门控 2 临时小段 MergeRuns 合并）
```

- **内存限制器只决定「何时 flush 这个 memtable」**（它经 §2 的 MemTracker 桥接看得见 SNII 倒排内存）；
  **它不直接调 SNII**。真正落 `.idx` 是随 memtable flush **自然走到**倒排 writer 的 `finish()`——
  这是 Doris 既有的 segment 写出路径，SNII 只是接在该接口背后。
- 线程：`finish()` 跑在 flush 线程，此时该 memtable 已封口、**无并发 add_values**——无竞争。
- **SNII 侧**：实现/对接 `InvertedIndexColumnWriter` 背后的 `add_values→SpimiTermBuffer 喂入` 与
  `finish→finalize` 即可；finalize 内核已存在。

### 3.2 门控 2：倒排 buffer 上限（512M）→ 内部分段 spill

- **保留**现有内部阈值自触发（`spimi_term_buffer.cpp:159`），这就是门控 2。两处改动：
  1. **阈值接 Doris 配置**（如 512 MiB），不再是本地常量；经 `SniiIndexInput` 注入。
  2. **判定改用真实字节**：`arena_bytes()(+ slot_of_ + dict ram) >= 阈值` 触发 spill，
     而非 `live_bytes_` 估计（与 §2.2 同源）。
- spill 一个小段到临时目录（`spill_to_run`，可重入），reset arena + 报负 delta（释放被 Doris 看见），
  继续累积。**自触发、在累积线程内**——沿用现有安全模型，无跨线程。
- segment flush（门控 1 的 finalize）时 `MergeRuns` 把所有小段 + 残留合并成最终 `.idx`，**byte-identical**。
- 4 GiB arena 兜底（`kArenaSpillCap`，`:158`）保留为格式正确性下限，与上面正交。

> 「小段」的字节格式：现状是私有 raw-u32 run（`spill_run_codec.h`，build-内部、最终 merge 成 `.idx`），
> 满足「内部分段 + k-way merge」。是否升级为 native idx 段是后续优化，与两门控逻辑无关。

---

## 4. 为什么这版同时消解了两轮评审的问题

| 评审发现 | 本版为何不存在 / 已修 |
|---|---|
| 预算器 victim / 双减 / 锁内 spill（第一轮） | 无预算器、无 victim、无 budget 减法——只一个计数器报真实增减 |
| 「Doris 跨线程调 flush() 动正在累积的 buffer」UAF（第二轮） | **门控 1 是封口后 finalize**（无并发）；**门控 2 是累积线程内自触发**（现有安全模型）——无跨线程动 buffer |
| 「Doris 无 flush 钩子」「已有 native SPIMI」（第二轮的两条 Doris 前提） | **已撤回**——真实 flush 在 `load/memtable/MemTableMemoryLimiter`（非 `runtime/memory` 的 CANCEL）；`inverted/spimi/` 是样本临时代码、非现行实现 |
| 统计报 `live_bytes_` 在默认模式恒 0（第二轮，真 bug） | **已修**：改报 `arena_bytes()` + `slot_of_` + dict ram（真实字节，§2.2） |

---

## 5. 正确性与验收

1. **统计准确性（对照真实测量，非自报之和）**：构造已知增减序列，断言 `current_bytes()` 与
   **真实测量**（`arena_bytes()` + `slot_of_.capacity()*4` + dict `ram_bytes_`）一致；
   覆盖 spill 归还 arena、`merge_runs` 释放 `slot_of_`、dict 转盘三处必报负 delta（无泄漏）。
2. **门控 2 等价产物**：不同 512M 触发时序（含纯内存不 spill）下最终 `.idx` **逐字节一致**
   （`spill_to_run`+`MergeRuns` 已保证）。
3. **门控 2 真实字节触发**：设 512M 阈值，构造内存恰好越线，断言用 `arena_bytes` 口径触发 spill。
4. **零成本观测**：`report` 每次一个 relaxed atomic add + 可选回调；构建 CPU 无显著回归。

---

## 6. 实施阶段

1. **P1**：`memory_reporter.h` + 各模块报**真实字节**（`arena_bytes` / `slot_of_` / dict ram）+
   统计准确性单测（对照真实测量）。
2. **P2**：门控 2 改造——阈值经 `SniiIndexInput` 接配置（512M），触发判定改用 `arena_bytes` 口径
   （替掉 `live_bytes_`）；保留 4 GiB 兜底。
3. **P3**：与 Doris 对接——① `consume_release_` 桥接 load MemTracker（门控 1 可见性）；
   ② 让 SNII 坐在 `InvertedIndexColumnWriter`（`add_values`/`finish`）背后，使既有的
   memtable flush → segment 写出 → `finish()` 自然落到 SNII finalize（**不**直接接 memory limiter）。
   **此阶段在合并侧。**
4. **P4（可选）**：内部分段升级为 native idx 段。

---

## 7. 结论（一句话）

**两个独立门控：门控 1（Doris `MemTableMemoryLimiter` 软/硬限）触发整段 `finalize()` 产出完整 `.idx`，
SNII 只需把内存准确报进 Doris load tracker 让它看得见；门控 2（倒排自身 512M buffer 上限）是 SNII
自己在累积线程内触发的内部分段 spill + segment flush 时 k-way merge——这是现有机制，只需把阈值接配置、
并把内存口径从 `live_bytes_` 估计改成 `arena_bytes()` 真实值。** 无预算器、无 victim、无跨线程 flush。
