# 设计：构建期共享内存预算器（spill 触发从 per-buffer 改为进程级）

Status: draft（待评审）
Owner: SNII writer
Scope: `core/include/snii/writer/spimi_term_buffer.{h,cpp}`、
`core/include/snii/writer/spillable_byte_buffer.h`、`core/include/snii/writer/logical_index_writer.{h,cpp}`、
`SniiIndexInput`，新增 `core/include/snii/writer/build_memory_budget.h`

---

## 0. 背景与问题（Why）

SNII 构建期有两类可 spill 的内存消费者，**各自用「本 buffer 本地累计」决定何时 spill**：

| 消费者 | 追踪量 | 触发条件 | 出处 |
|---|---|---|---|
| `SpimiTermBuffer`（输入倒排累积） | `live_bytes_`（逐 token 累加） | `live_bytes_ >= spill_threshold_bytes_` | `spimi_term_buffer.cpp:159` |
| `SpillableByteBuffer`（dict 区缓冲） | `ram_bytes_`（逐 chunk 累加） | `ram_bytes_ >= cap_bytes_`（`SNII_DICT_RAM_MAX`） | `spillable_byte_buffer.h:53,69` |

二者都是**每 buffer 独立阈值，无共享预算**（代码中无 `MemTracker`/全局内存统计参与 spill 判定）。

**问题**：当一个 segment 有多个 logical index（多列），且调用方**同时持有**多个
`SpimiTermBuffer`（如 Doris 多列并行 ingest，build 前各列 token 已驻留），或未来**并行构建**多个
`LogicalIndexWriter`（各有一个 `dict_buf_`）时：

```
进程峰值 RSS ≈ Σ_over_buffers ( 各 buffer 本地阈值 + 瞬态 )
```

即**总内存 = 各 buffer 预算之和，不受任何单一上限约束**。N 列 × 每列阈值 → 峰值可达 N 倍。
`SNII_DICT_RAM_MAX` / `--spill-mib` 都是 per-buffer 值，**不是 segment/进程级内存预算**。

> 现状缓解：当前**串行构建**（`SniiCompoundWriter::add_logical_index` → `liw->build()` 跑完才下一个，
> `snii_compound_writer.cpp:48`），故同一时刻通常只有一个 `dict_buf_` 活跃；bench 的 SPIMI buffer
> 也逐列建-drain-释放。**但这只是调用方时序的偶然结果，不是机制保证**——通用调用方多列同时持有
> SPIMI buffer，或引入并行构建后，Σ 立刻失控。

**目标**：引入一个**进程级共享内存预算器** `BuildMemoryBudget`，让所有可 spill 消费者向**同一个预算**计费；
当全局占用逼近上限时，由预算器**择最大贡献者 spill**，使**整个构建进程的常驻内存有单一上限**，
与 buffer 数量、列数、是否并行无关。

---

## 1. 现状与 spill 内核（可复用的基础）

两个 spill 内核已存在，预算器只需驱动它们，不重写：

- **SPIMI `spill_to_run()`（可重入）**：把当前 terms 落一个 raw-u32 run 文件，`pool_.reset()` 归还 arena +
  `TrimMalloc()`，`live_bytes_ = 0` 后继续累积下一批；末尾 k-way merge 合并所有 run + 末批内存项。
  即**可多次 spill**，每次释放近乎全部常驻（`spimi_term_buffer.cpp:421-429,451`）。输出 byte-identical。
- **dict `SpillableByteBuffer::spill_to_disk()`（一次性）**：把已累积 chunk 链写入 temp，之后**所有 append
  改道 temp**，常驻 → 0（`spillable_byte_buffer.h:106-118`）。即**一次 spill 后不再是预算贡献者**。

两者「spill 后释放多少常驻」不同，预算器的择victim 逻辑需感知（见 §3.3）。

---

## 2. 设计总览（What）

```
┌──────────────────────── 调用方（构建一个 segment 的编排者） ───────────────────────┐
│  创建 ONE BuildMemoryBudget(limit)，传入每个 SniiIndexInput / SpimiTermBuffer       │
└───────────────────────────────────┬─────────────────────────────────────────────────┘
                                     │ BuildMemoryBudget*（共享，非拥有）
        ┌────────────────────────────┼────────────────────────────┐
        │                            │                            │
┌────────▼─────────┐      ┌───────────▼──────────┐      ┌──────────▼───────────┐
│ SpimiTermBuffer  │      │ SpillableByteBuffer  │      │ …（未来其它可 spill）│
│  : Spillable     │      │  (dict_buf_) :Spillable│     │                      │
│ resident_bytes() │      │ resident_bytes()      │      │                      │
│ spill_some()     │      │ spill_some()          │      │                      │
└────────┬─────────┘      └───────────┬───────────┘      └──────────────────────┘
         │ charge(delta)              │ charge(delta)
         └──────────────┬─────────────┘
                        ▼
        ┌──────────────────────────────────────────┐
        │ BuildMemoryBudget                          │
        │  atomic total_；registry of Spillable*     │
        │  charge(delta, self): total_+=delta；       │
        │    while total_ > limit_:                  │
        │      victim = 最大 resident 的 Spillable    │
        │      freed = victim->spill_some()          │
        │      if freed==0: break（无法再降，放行）   │
        └──────────────────────────────────────────┘
```

要点：
1. **可 spill 消费者实现统一接口 `Spillable`**，构造时向预算器 `register`，析构时 `unregister`。
2. **计费**：buffer 每次常驻字节变化（token 累积 / chunk 追加 / spill 释放）调
   `budget->charge(delta, this)`。
3. **触发**：当全局 `total_` 越过 `limit_`，预算器**择当前 `resident_bytes()` 最大的 victim** 调
   `spill_some()`，循环直到回到上限内或无人能再释放（避免死循环）。
4. **向后兼容**：`budget == nullptr` 时退回**现有 per-buffer 阈值行为**（零改动语义）；
   预算器存在时，per-buffer 阈值作为**本地硬上限**仍保留（双闸：全局预算 + 本地兜底）。

---

## 3. 接口设计（实现就绪）

### 3.1 `Spillable` 接口

```cpp
// core/include/snii/writer/build_memory_budget.h
namespace snii::writer {

// A build-time RAM consumer that can shed resident memory on demand. Registered with
// a BuildMemoryBudget so the budget can pick it as a spill victim under pressure.
class Spillable {
 public:
  virtual ~Spillable() = default;
  // Current resident byte estimate this consumer holds (the budget's victim metric).
  virtual uint64_t resident_bytes() const = 0;
  // Shed as much resident memory as is reasonable in one step (drain to disk, reset
  // an arena, etc.). Returns the number of bytes freed (0 if it cannot free more, so
  // the budget stops choosing it). Must be safe to call between, not during, the
  // consumer's own append unit (the budget serializes via the same lock -- see §4).
  virtual Status spill_some(uint64_t* freed) = 0;
};

}  // namespace snii::writer
```

### 3.2 `BuildMemoryBudget`

```cpp
class BuildMemoryBudget {
 public:
  // limit_bytes == 0 disables the global budget (consumers fall back to their own
  // per-buffer thresholds -- the current behavior). Shared by all consumers of one
  // segment build; the caller owns it and keeps it alive across the build.
  explicit BuildMemoryBudget(uint64_t limit_bytes);

  void register_spillable(Spillable* s);     // on consumer construction
  void unregister_spillable(Spillable* s);   // on consumer destruction (also releases its charge)

  // Record a resident-size change of `self` (+grow / -shrink). If the new global total
  // exceeds limit_bytes, drive victims to spill_some() until total <= limit or no
  // victim can free more. Returns a spill/merge I/O error verbatim (latched by caller).
  Status charge(int64_t delta, Spillable* self);

  uint64_t total() const;   // current global resident estimate
  uint64_t limit() const;
  bool enabled() const { return limit_ != 0; }

 private:
  Status relieve_locked();  // victim loop under lock_

  const uint64_t limit_;
  mutable std::mutex lock_;
  uint64_t total_ = 0;
  std::vector<Spillable*> registry_;  // non-owning
};
```

`charge` 核心逻辑：

```cpp
Status BuildMemoryBudget::charge(int64_t delta, Spillable* /*self*/) {
  if (limit_ == 0) return Status::OK();
  std::lock_guard<std::mutex> g(lock_);
  total_ = static_cast<uint64_t>(static_cast<int64_t>(total_) + delta);
  if (total_ <= limit_) return Status::OK();
  return relieve_locked();
}

Status BuildMemoryBudget::relieve_locked() {
  while (total_ > limit_) {
    Spillable* victim = nullptr;
    uint64_t best = 0;
    for (Spillable* s : registry_) {
      const uint64_t r = s->resident_bytes();
      if (r > best) { best = r; victim = s; }
    }
    if (victim == nullptr || best == 0) break;  // nobody can free -> let it ride
    uint64_t freed = 0;
    SNII_RETURN_IF_ERROR(victim->spill_some(&freed));
    if (freed == 0) break;                       // victim cannot shrink further
    total_ -= std::min(freed, total_);
  }
  return Status::OK();
}
```

> **择 victim = 最大常驻** 而非 self-spill：多 buffer 场景下先 spill 最大者回收最多内存，
> 避免「小 buffer 撞线却让大 buffer 常驻」。`freed==0` 兜底防死循环（例如所有 victim 都已 spill 干净）。

### 3.3 两个消费者的接入

**`SpimiTermBuffer : public Spillable`**
- `resident_bytes()` 返回 `live_bytes_`。
- `spill_some(freed)`：调现有 `spill_to_run()`；`*freed = (spill 前 live_bytes_)`；spill 后 `live_bytes_=0`。
  **可重入**——下次仍可被选中。
- `account_token` 累积处（`spimi_term_buffer.cpp:159` 附近）：原 `live_bytes_ >= threshold` 自检**之外**，
  把 `live_bytes_` 的增量 `charge` 给预算器。本地 `spill_threshold_bytes_` 作为兜底硬上限保留。

**`SpillableByteBuffer : public Spillable`**
- `resident_bytes()` 返回 `spilled_ ? 0 : ram_bytes_`（已 spill 后不再贡献）。
- `spill_some(freed)`：若未 spill，调 `spill_to_disk()`，`*freed = ram_bytes_`（spill 前），之后常驻 0；
  已 spill 则 `*freed = 0`。**一次性**——之后 `resident_bytes()=0`，预算器不再选它。
- `append`/`append_move`（`:51,66`）：`ram_bytes_` 增量 `charge` 给预算器；本地 `cap_bytes_` 兜底保留。

### 3.4 注入路径

- `SniiIndexInput` 增字段 `BuildMemoryBudget* mem_budget = nullptr;`（非拥有）。
- `LogicalIndexWriter` 构造时把 `in.mem_budget` 传给 `dict_buf_`（`SpillableByteBuffer` 增一个
  可选 `BuildMemoryBudget*` 参数）。
- 调用方创建 `SpimiTermBuffer` 时同样传入同一个 `BuildMemoryBudget*`。
- 编排者（构建一个 segment 的顶层）持有**唯一**的 `BuildMemoryBudget`，贯穿整个 build。

---

## 4. 线程安全契约（为未来并行构建预留）

当前**串行构建**，但预算器的价值正是约束「多 buffer 并存 / 并行」，故按线程安全设计：

- `BuildMemoryBudget` 用 `std::mutex lock_` 守 `total_` + `registry_`；`charge`/`relieve_locked`/`register`/
  `unregister` 全程持锁。
- **spill I/O 在锁内执行**：这给出天然 backpressure——内存压力事件（罕见）期间，其它线程的 `charge`
  阻塞等待 victim spill 完成。构建非延迟敏感，可接受。
- 每个 `Spillable` 的 `append` 单元（实际 memcpy）在锁外完成、**仅 `charge` 进锁**；为避免 victim 在被
  spill 时正被自身 append，规定：**`spill_some()` 与该 buffer 自身的 append 必须互斥**。串行构建下天然满足；
  并行构建下，每个 Spillable 用自身的 `std::mutex` 守其常驻状态，`spill_some` 与 `append` 各取该锁
  （v1 可先只支持串行，§7 标注）。

> **v1 范围**：目标是「串行构建下多列同时持有 SPIMI buffer」与「单测可触发」的进程级约束。
> 真正的并行构建（多线程同时 build 多个 LogicalIndexWriter）作为 v2，在 §4 的 per-Spillable 锁完成后开启。

---

## 5. 配置

- 新环境变量 `SNII_BUILD_MEM_BUDGET`（字节，0/缺省 = 关闭 → 退回 per-buffer 阈值，**当前行为**）。
- 与 per-buffer 阈值的关系（双闸，取**先到者**）：
  - 全局预算 `limit_`：跨所有 buffer 的 Σ 上限（**进程级**）。
  - 本地 `spill_threshold_bytes_` / `cap_bytes_`：单 buffer 兜底（防单个超大 term/dict 撑爆）。
- 建议：编排者按「可用内存 × 系数」设 `limit_`，各 buffer 本地阈值设为 `limit_` 或略大（让全局预算主导）。

---

## 6. 正确性与验收

1. **输出 byte-identical**：spill 时机不影响产物（SPIMI 与 dict 的 spill 都已保证 byte-identical）。
   预算器只改**何时**spill，不改**写什么**。回归：开/关预算器 + 不同 `limit_`，产物 `.idx` 逐字节一致。
2. **进程级上限生效**：单测构造 N 个 `SpimiTermBuffer` + 多个 `SpillableByteBuffer` 共享一个小 `limit_`，
   持续喂数据，断言 `budget.total()` 始终 `<= limit_ + 单个最大 append 单元`（允许瞬时越界一个 append 量，
   下一 charge 即回收）。
3. **victim 正确性**：构造大小悬殊的多个 buffer，断言预算器优先 spill 最大者（`spill_some` 调用计数/顺序）。
4. **关闭等价**：`limit_=0` 时三黄金指标 + 产物与现状完全一致（回退路径无回归）。
5. **死循环防护**：所有 victim 已 spill 干净仍越界（单个 term/dict 本身 > limit）时不挂起（`freed==0` 退出）。

---

## 7. 实施阶段

1. **P1**：新增 `build_memory_budget.h`（`Spillable` + `BuildMemoryBudget`）+ 单测（victim 选择、计费、死循环防护）。
2. **P2**：`SpillableByteBuffer` 实现 `Spillable` + 可选 budget 接入；dict 路径单测（共享小预算触发 dict spill，
   产物 byte-identical）。
3. **P3**：`SpimiTermBuffer` 实现 `Spillable` + 接入；`SniiIndexInput.mem_budget` 注入 `LogicalIndexWriter`。
4. **P4**：bench / 顶层编排创建共享 budget；`--build-mem-budget` 旗标；多列 RSS 实测（对比 per-buffer Σ）。
5. **P5（v2）**：per-Spillable 锁，开启并行构建。

---

## 8. 风险与权衡

| # | 风险 | 缓解 |
|---|---|---|
| R1 | spill I/O 在全局锁内 → 压力期串行化所有 charge | 构建非延迟敏感；压力事件罕见；这正是期望的 backpressure。可后续把 spill 移出锁 + 重验 total |
| R2 | victim 频繁 spill 抖动（刚 spill 又撞线） | dict 一次性 spill 后即退出贡献；SPIMI spill 释放近全部常驻，单次回收大、抖动有限。必要时加滞后（spill 到 `limit_ * 0.8`） |
| R3 | `resident_bytes()` 是估计值（live_bytes_ 用固定 per-node 系数），与真实 RSS 有偏差 | 与现有 per-buffer 阈值同源估计，偏差一致；预算器约束的是「估计 Σ」，与现状口径一致，足够 bound |
| R4 | 单个 term/dict 本身 > limit_ | `freed==0` 放行，回退到本地硬上限（arena 4GiB 兜底等）；预算器不承诺 < 单个不可分项 |
| R5 | 计费遗漏（某路径改了常驻却没 charge） | 接入点集中在 append/account_token/spill 三处；单测 #2 用 `total()` 守恒交叉校验 |

---

## 9. 结论（一句话）

**新增一个进程级 `BuildMemoryBudget`：所有可 spill 消费者（SPIMI 输入 buffer、dict 缓冲）实现统一
`Spillable` 接口并向同一预算计费，越过全局上限时由预算器择最大贡献者 spill——把 spill 触发从
「每 buffer 本地阈值之和无上限」改为「进程级单一上限」，且 `limit=0` 完全退回现状、产物 byte-identical。**
