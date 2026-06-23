# 设计：构建期全局内存统计 + 外部下刷 + idx 分段 k-way merge

Status: draft（待评审）
Owner: SNII writer
Scope: `core/include/snii/writer/spimi_term_buffer.{h,cpp}`、`compact_posting_pool.h`、
`spillable_byte_buffer.h`、`logical_index_writer.{h,cpp}`、`SniiIndexInput`，
新增 `core/include/snii/writer/memory_reporter.h`；与 Doris memory manager 的集成边界。

> **本设计取代** 早前的「共享内存预算器（victim 选择 / charge / 预算器自做 spill）」方案。
> 那一版把「下刷决策」放进 SNII，导致 victim 选择、计费 double-decrement、生命周期悬垂、
> 锁内 spill 等一系列 blocker（见对抗评审）。**职责放错了位置**。本设计把职责摆正：
> **SNII 只做准确统计 + 被动下刷 + 合并；下刷决策由 Doris 全局内存管理掌控。**

---

## 0. 目标与职责边界（Why）

粒度是 **writer 级**：一个 SNII writer = 一个 `SniiCompoundWriter` = 一个 segment 的倒排索引 =
**一个下刷单元**。一个 BE 进程里同时有很多 writer 在跑（多个导入 / tablet / compaction 任务），
真正的并发与内存压力来自**进程内多个 writer 之间**（单个 writer 内部是串行的）。

完整闭环（你描述的标准 Doris 内存回收路径）：

```
┌─────────────────────── Doris 内存管理（policy / 决策） ──────────────────────────────┐
│  ① 每个 writer 把自己的内存计入 MemTracker 层级节点                                    │
│  ② MemTracker 汇总到进程级 → 内存过高触发 SOFT LIMIT                                   │
│  ③ MemoryReclamation 尝试下刷：memtable + 倒排索引                                     │
│  ④ 对内存大的 writer 节点，调它的 flush()（决策「刷谁」在 Doris，它有 per-writer 数据） │
└───────────────────┬──────────────────────────────────────────────────────────────────┘
        ① consume/release（writer 级上报）   ④ 触发 flush()
┌────────────────────▼──────────────────────────────────────────────────────────────────┐
│  SNII writer（mechanism / 执行，per segment）                                            │
│   (1) writer 内各模块准确自报内存 → 汇总成该 writer 的 current_bytes，并 consume 到       │
│       Doris MemTracker 对应节点（纯观测，不决策）                                         │
│   (2) flush()：被 Doris 触发时，把当前累积**下刷一个 idx 分段到临时文件目录** + reset      │
│       （可重入：内存压力期可多次 flush，产生多个临时分段）                                │
│   (3) segment flush（finalize）：把所有临时 idx 分段 + 残留 **k-way merge** 成该 segment   │
│       的最终倒排索引                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────────┘
```

三件事，**仅此而已**：

1. **writer 级准确内存统计**：一个 writer 内各模块自报自己的内存，汇总成该 writer 的占用，
   并 consume 到 Doris MemTracker 节点；MemTracker 自然 roll-up 到进程级。**SNII 不做下刷决策**。
2. **外部下刷到临时分段**：Doris 触发 soft limit → MemoryReclamation 对内存大的 writer 调 `flush()`
   → SNII 把当前累积**落一个 idx 分段到临时目录**（可重入，多次 flush = 多个临时分段）。
3. **segment flush 时 k-way merge**：该 segment 真正 flush（所有行就绪）时，把临时目录里的所有 idx
   分段 + 内存残留 **k-way merge** 成最终倒排索引。

**职责边界**：「刷谁、何时刷」= Doris（它有 per-writer MemTracker 数据 + 全局 soft limit）；
「怎么落分段、怎么 merge」= SNII。

**非目标（明确删除）**：SNII **不**实现 victim 选择、共享预算器、charge/relieve、跨 writer 协调、
锁内 spill。这些是 Doris MemTracker / MemoryReclamation 的职责，SNII 重复实现只会与之打架。

---

## 1. 现状：素材已齐，缺的是「上报」与「外部触发」

| 能力 | 现状 | 出处 |
|---|---|---|
| 各模块自报字节 | **已有**：`CompactPostingPool::arena_bytes()`、`SpimiTermBuffer::live_bytes_`、`SpillableByteBuffer::size()`（`ram_bytes_`） | `compact_posting_pool.h:105`、`spimi_term_buffer.h:288`、`spillable_byte_buffer.h:40` |
| 下刷成段的内核 | **已有**：`spill_to_run()`（可重入，落一个 run、reset arena、继续） | `spimi_term_buffer.cpp:451` |
| k-way merge | **已有**：`MergeRuns()` | `spill_run_codec.h:177` |
| **全局内存统计上报** | **缺** | 本设计 §2 |
| **外部 flush 触发** | **缺**（现仅内部阈值 `live_bytes_ >= spill_threshold_bytes_` 自触发，`spimi_term_buffer.cpp:159`） | 本设计 §3 |
| 下刷段格式 | 私有 run（raw u32，仅本模块读写，**非 native idx 段**） | `spill_run_codec.h:14-23` |

净新增只有两点：**(1) 全局内存统计上报**、**(2) 外部 flush 触发**（把触发权从 SNII 内部阈值
移交 Doris）。下刷内核与 k-way merge 复用现有。

---

## 2. 准确全局内存统计（核心）

### 2.1 `MemoryReporter`：极简全局累加器

```cpp
// core/include/snii/writer/memory_reporter.h
namespace snii::writer {

// Per-WRITER accurate byte counter for build-time RAM (one per SniiCompoundWriter =
// one per segment's inverted index). Every module inside that writer REPORTS its own
// resident-byte changes here; current_bytes() is that writer's accurate live usage,
// which Doris reads and rolls up to process level via its MemTracker. OBSERVE-ONLY:
// SNII never makes a flush decision from it. Thread-safe (atomic); cheap on the hot
// path (one relaxed add + an optional bridge call).
class MemoryReporter {
 public:
  // consume_release: optional bridge to Doris's MemTracker node for this writer.
  // Called with the same delta on every report (delta>0 = consume, <0 = release).
  // Null when SNII runs without Doris (bench / unit tests) -> local atomic only.
  using ConsumeReleaseFn = std::function<void(int64_t delta)>;
  explicit MemoryReporter(ConsumeReleaseFn consume_release = nullptr)
      : consume_release_(std::move(consume_release)) {}

  // delta > 0 on growth, < 0 on shrink/free. Each module is the sole source of truth
  // for ITS OWN bytes; double-reporting is a module bug, not handled here.
  void report(int64_t delta) {
    current_.fetch_add(delta, std::memory_order_relaxed);
    if (consume_release_) consume_release_(delta);  // mirror into Doris MemTracker
  }
  int64_t current_bytes() const { return current_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> current_{0};
  ConsumeReleaseFn consume_release_;
};

}  // namespace snii::writer
```

> 为什么这次「简单」是对的：上一版把 `MemoryReporter` 升级成会**回调 victim、自己 spill** 的
> 预算器才出问题。这里它**只是一个 atomic 计数器**——report 加、读取看。没有锁、没有 registry、
> 没有 victim、没有生命周期悬垂。准确性只依赖「每个模块对自己的字节增减如实 report」。

### 2.2 各模块如何 report（自报，单一真值）

每个模块在**自己的内存增减处**调一次 `report(delta)`，与现有的自报量一一对应：

| 模块 | report 点 | delta |
|---|---|---|
| `CompactPostingPool` | arena 增长 / `reset()` | `arena_bytes()` 的增量 / 归还时负 delta |
| `SpimiTermBuffer` | `account_token`（`:159` 附近）/ drain / spill reset | `live_bytes_` 的增量；drain 或 spill 把 `live_bytes_` 归 0 时 report 对应负 delta |
| `SpillableByteBuffer` | `append`/`append_move`（`:51,66`）/ spill 后 | `ram_bytes_` 的增量；spill 到盘后 report `-ram_bytes_`（常驻转盘） |

**关键纪律（避免上一版的计费灾难）**：
- **每个模块只 report 自己**，且**每处内存增减恰好 report 一次**（增正、减负）。
- 没有「budget 减一次 + 模块再减一次」的双重路径——因为**这里根本没有 budget 去减**。
  减少只发生在「模块自己的字节真的变少」时（drain/spill/reset），由该模块 report 负 delta。
- drain（`for_each_term_sorted` 末尾 `live_bytes_=0`）也是一处 report 负 delta——上一版漏掉它造成
  泄漏；这里把它列为**必报点**。

### 2.3 粒度与 Doris MemTracker 的对接（集成边界）

**粒度 = writer 级**：每个 `SniiCompoundWriter` 持有一个 `MemoryReporter`；该 writer 内的所有模块
（各列的 `SpimiTermBuffer` / `dict_buf_` / `CompactPostingPool`）都 report 到**这一个** reporter。
于是：

```
writer.current_bytes() == 该 segment 倒排索引构建的实时常驻字节   ← Doris 查「这个 writer 用了多少」
```

对接 Doris MemTracker（SNII core 不直接依赖 Doris 类型，用一个回调桥接）：

- `MemoryReporter` 持一个可选的 `consume_release_fn`（合并侧注入）。`report(delta)` 时既更新本地
  atomic，又调 `consume_release_fn(delta)` 把该 writer 的内存 **consume/release 到 Doris 对应的
  MemTracker 节点**。
- Doris MemTracker 自然 **roll-up 到进程级**；`GlobalMemoryArbitrator` 看进程总量触发 **soft limit**；
  `MemoryReclamation` 看各 writer 节点（per-writer 数据）决定**对哪个 writer 调 `flush()`**。

即「实时查询」与「挑谁 flush」共用**同一套 per-writer MemTracker 节点**，SNII 不维护第二份。
未连 Doris（如 bench / 单测）时 `consume_release_fn` 为空，只走本地 atomic，`current_bytes()` 仍可读。

### 2.4 旧内部阈值的处置

- `spill_threshold_bytes_` / `SNII_DICT_RAM_MAX` **不再驱动常规下刷**（下刷改由外部 flush 驱动）。
- 保留为**硬安全兜底**：arena 逼近 4 GiB uint32 偏移上限时仍强制 spill（`kArenaSpillCap`，
  `spimi_term_buffer.cpp:158`）——这是格式正确性下限，与内存管理正交。

---

## 3. 外部下刷 API + idx 分段

### 3.1 `flush()`：把触发权交给外部（时序）

下刷与最终合并发生在**两个不同时刻**，对应 Doris 的两个动作：

```
  导入中（行不断进入，倒排累积在内存）
     │
     ├──[内存压力] Doris soft limit → MemoryReclamation 对该 writer 调 flush()
     │      → 把当前累积落一个 idx 分段到【临时目录】+ reset，继续累积   ← 可重复多次
     │
     └──[segment 真正 flush，所有行就绪] Doris 调 finalize/close
            → 把临时目录里所有 idx 分段 + 内存残留 k-way merge 成该 segment 的最终倒排索引
```

```cpp
// 由 SniiCompoundWriter 暴露、透传到各列的 SpimiTermBuffer（公开）
// Externally triggered by Doris under memory soft-limit: flush this writer's CURRENT
// resident accumulation as ONE idx segment into the temp directory and reset, so
// accumulation continues with freed RAM. Re-entrant (each flush -> one more temp
// segment). Does NOT produce the final index -- that happens at finalize via k-way
// merge. SNII does NOT self-trigger flush (except the 4 GiB arena hard-stop).
Status flush();
```

- **flush() 时**：复用现有 `spill_to_run()`（已可重入：落一个临时分段、reset arena、
  `live_bytes_=0` 并 report 负 delta、继续累积）。RAM 被释放（report 负 delta → Doris MemTracker
  release），缓解进程内存压力。
- **finalize（segment flush）时**：复用现有 `merge_runs()` / `MergeRuns()`，把所有临时分段 +
  残留 k-way merge 成最终倒排索引（已保证 byte-identical，与 flush 次数/时机无关）。
- `account_token` 内**移除**阈值自触发（只留 4 GiB 兜底）；常规下刷时机完全由 Doris 决定。
- `LogicalIndexWriter` / `SniiCompoundWriter` 透传 `flush()`，使 Doris 能对**一个具体 writer**
  （内存大的那个）精准下刷。

### 3.2 idx 统一分段：两种粒度

「下刷成什么段」有两种选择：

- **(a) 复用现有私有 run 段（推荐起步）**：每次 `flush()` 落一个私有 run（raw u32，I/O 便宜），
  finalize 时 `MergeRuns` k-way merge 成单一 idx。**已具备「分段 + k-way merge」**，净改动仅
  「外部触发」。满足「构建期内存受 Doris 全局控制」的核心诉求。
- **(b) native idx 分段（对齐 Doris segment/compaction，后续）**：每次 `flush()` 产出一个**可独立
  读的 mini-idx segment**，k-way merge 在 idx 层合并（甚至合并前可作为多段被查询）。更贴合 Doris
  的 memtable-flush→segment→compaction 模型，但工作量大（spill 格式升级为 native、merge 升级为
  idx 级）。

**建议**：P 阶段先 (a)（最小改动、复用全部现有内核），(b) 作为与 Doris compaction 完全对齐的后续。
两者都满足「idx 文件统一分段 + k-way merge」的形态；区别只是分段的**字节格式**（私有 raw vs native idx）。

---

## 4. 为什么这版消解了对抗评审的全部 blocker

| 上一版 blocker | 本版为何不存在 |
|---|---|
| B1 择 victim 误 spill 正在 drain 的 buffer | **无 victim 选择**——SNII 不挑谁 spill；flush 由外部对**指定** writer 调用，不会去动正在 drain 的内部 buffer |
| B2 计费 double-decrement / uint64 下溢 | **无 budget 做减法**——只有模块自己 report 自己的真实增减；无「budget 减 + 模块减」双路径 |
| B3 / B9 registry 悬垂 / 单预算强制 | **无 registry、无 Spillable 注册**——只有一个 atomic 计数器；模块 report 自己即可 |
| B6 `spill_some` 全有或全无 → over-spill | **不存在 victim 取舍**——flush 就是把当前累积整体落段（本就是设计意图），不是为缓解 1MiB 去 spill 60MiB |
| B7 spill I/O 在全局锁内 | **无全局锁**——`report` 是 relaxed atomic add；flush 各自执行，互不阻塞 |
| B8 并发 TOCTOU 超额 | `current_bytes` 是事后准确统计供 Doris 决策；Doris 的全局裁决本就容忍瞬时波动，不承诺硬实时上限 |

本质：**把「决策」搬出 SNII**，那些 blocker 全是「SNII 自己做下刷决策」才会有的复杂度。

---

## 5. 正确性与验收

1. **统计准确性**：构造已知的内存增减序列（累 token / 追加 chunk / drain / spill / reset），
   断言 `current_bytes()` 与「各模块自报量之和」始终一致（增减成对，无泄漏/无双减）。
   尤其覆盖 drain 末尾 `live_bytes_=0`、spill 后 `ram_bytes_→盘` 两处必报负 delta。
2. **外部 flush 等价产物**：在不同的外部 flush 时序（含全程不 flush 的纯内存路径）下，最终 `.idx`
   **逐字节一致**（下刷只改内存峰值，不改产物——现有 spill/merge 已保证 byte-identical）。
3. **无内部阈值驱动**：除 4 GiB 兜底外，构建过程不因任何本地阈值自发 spill（外部不调 flush 则不 spill）。
4. **零成本观测**：`report` 在 hot path（每 token）只一次 relaxed atomic add；基准对比构建 CPU 无显著回归。

---

## 6. 实施阶段

1. **P1**：`memory_reporter.h`（atomic 计数器）+ 各模块接 `report`（pool / spimi / dict）+ 统计准确性单测。
2. **P2**：`SpimiTermBuffer::flush()` 外部 API（复用 `spill_to_run`）；移除 `account_token` 内阈值自触发
   （保留 4 GiB 兜底）；`LogicalIndexWriter`/`SniiCompoundWriter` 透传 flush。
3. **P3**：与 Doris 对接（§2.3 的 A 或 B）——`report` 桥接 MemTracker 或暴露 `current_bytes()` 给
   `MemoryReclamation`，由 `GlobalMemoryArbitrator` 调 flush。**此阶段在合并侧。**
4. **P4（可选）**：native idx 分段（§3.2(b)），对齐 Doris memtable-flush/compaction。

---

## 7. 结论（一句话）

**SNII 只提供三件简单的事——各模块准确自报内存（汇总成一个 atomic 全局值，纯观测）、被外部触发的
`flush()`（复用现有 spill 内核把当前累积落成统一 idx 分段）、以及现有的 k-way merge 合并分段；
下刷决策完全交给 Doris 的 memory allocator / memory manager 全局掌控。** 不做 victim、不做预算器、
不做锁内 spill——那些是上一版放错职责才需要的复杂度。
