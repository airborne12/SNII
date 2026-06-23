# 设计：将 Doris `query_v2` 查询算子适配到 SNII 格式

Status: draft（合并前规划，待评审）
Owner: SNII reader / query
Scope: Doris `be/src/storage/index/inverted/query_v2/*`、SNII `core/include/snii/reader/*`、
`core/include/snii/io/batch_range_fetcher.h`、SNII `core/src/query/*`（现有算子，将被替换）

> 代码路径基准：
> - Doris query_v2：`/mnt/disk1/jiangkai/workspace/src/doris-clean/be/src/storage/index/inverted/query_v2/`
> - SNII reader/io/query：`core/include/snii/{reader,io,format}/*`、`core/src/query/*`

---

## 0. 背景与目标（Why）

SNII 与 Doris 合并后，SNII 自带的 `core/src/query/`（`term_query` / `phrase_query` /
`scoring_query` / `boolean_and`）将被 **Doris `query_v2`** 的查询算子取代。`query_v2`
已实现完整的算子族（term / boolean / phrase / phrase_prefix / prefix / wildcard /
regexp / wand / 交并差），但它**默认绑定 CLucene 的读取模型**——一套
**惰性拉取迭代器**（lazy pull iterator）。

SNII 的核心价值是**对象存储读路径的三个黄金指标**（设计文档 §评估指标）：

| 指标 | 含义 | 影响 |
|---|---|---|
| **串行 I/O 轮次** | 后续读依赖前批返回而形成的顺序等待轮数 | 查询延迟下限、p99 |
| **Range GET 数** | 远端 range 请求数量 | 请求计费、远端 QPS |
| **读取字节数** | 所有 range 返回的总字节量 | 带宽、缓存占用、解码 CPU |

实测（5M docs，真实 OSS）：SNII 短语 / MATCH_PHRASE_PREFIX 凭**更少串行轮次**取得
**6–10× 延迟优势**；这正是 SNII「**先规划全部 range，再一轮并发取**」模型的产物。

**本设计的目标**：让 `query_v2` 的算子读取 SNII 格式，**同时保持 SNII 的批量-并发读模型
（三个黄金指标不退化）**。若把 `query_v2` 的惰性迭代器直接套一个「每 `readBlock` 调一次
`read_at`」的适配器，会让串行轮次随 block 数量线性爆炸，**彻底摧毁 SNII 的延迟优势**——
这是本设计要规避的头号失败模式。

**非目标**：本设计**不做** lazy/skip 位置解码等 SNII 现有算子内部优化（合并后这些逻辑由
`query_v2` 承载，应在 `query_v2` 侧按 SNII 格式重做，见 §6）。

---

## 1. 两套读取模型对比（核心张力）

### 1.1 `query_v2`：惰性拉取迭代器

`query_v2` 的算子全部编程于一组**迭代器接口**之上（`doc_set.h:29-65`、`scorer.h:46-74`、
`segment_postings.h:33-44`）：

```cpp
// doc_set.h —— 所有算子驱动文档迭代的契约
virtual uint32_t advance();              // 推进到下一个匹配 doc（惰性拉取）
virtual uint32_t seek(uint32_t target);  // 跳到第一个 doc >= target（随机访问）
virtual uint32_t doc() const;            // 当前 docid（耗尽时 TERMINATED=INT_MAX）
virtual uint32_t size_hint() const;      // 基数估计（用于查询规划）
virtual uint32_t freq() const;           // 当前 doc 的词频（评分用）
virtual uint32_t norm() const;           // 当前 doc 的 norm（评分用）
```

叶子 `SegmentPostings`（`segment_postings.h:48-252`）把上述迭代翻译成对底层 reader 的
**块拉取**调用（绑定 CLucene `TermDocs`/`TermPositions`）：

```cpp
bool readBlock(DocRange* block);       // 拉下一块：填 doc_many / freq_many / norm_many / doc_many_size_
bool skipToBlock(uint32_t target);     // 跳到含 docid>=target 的块
int32_t getMaxBlockFreq();             // 当前块最大词频（block-wand 剪枝）
int32_t getMaxBlockNorm();             // 当前块最大 norm（block-wand 剪枝）
int32_t getLastDocInBlock();           // 当前块最大 docid
int32_t nextDeltaPosition();           // 流式取下一个 delta 位置（短语）
void addLazySkipProxCount(int32_t n);  // 跳过 n 个位置不解码
```

**I/O 时机**：`readBlock` 在迭代器游标耗尽当前块时**按需触发**——I/O 随迭代**增量发生**。

### 1.2 SNII：批量规划-并发取

SNII 的每个算子（`core/src/query/*`）都遵循同一模式（见三份现有算子的读序）：

1. `LogicalIndexReader::lookup(term, …)` 解析词典 → `DictEntry` + `frq_base`/`prx_base`
   （内含 BSBF 快速拒绝）（`logical_index_reader.h:51-52`）；
2. 把本阶段需要的**所有字节 range** 用 `BatchRangeFetcher::add(off,len)` 登记
   （`batch_range_fetcher.h:27`）；
3. **一次** `fetch()` 合并 + 并发发出（`batch_range_fetcher.h:30`）→ **1 个串行轮次**；
4. 从 `get(handle)` 拿切片解码（`batch_range_fetcher.h:33`）。

**I/O 时机**：**先规划，后一轮并发取**。短语的多窗口、多词，尽量压进**同一轮**。

### 1.3 张力小结

| 维度 | query_v2（惰性） | SNII（批量） |
|---|---|---|
| I/O 触发 | 迭代游标耗尽时按需 | 阶段开始时一次性规划 |
| 串行轮次 | 易随 block/term 数线性增长 | 力求每阶段 1 轮 |
| 适配风险 | 朴素适配 = 每块一轮 = 延迟爆炸 | —— |

**适配的本质问题**：如何在**不改 `query_v2` 算子**的前提下，让其叶子 `SegmentPostings`
的块来源由 SNII 提供，并把「惰性 `readBlock`」收敛回「批量 1 轮取」。

---

## 2. 关键洞察：SNII「窗口」≡ query_v2「block」

SNII 的 **windowed posting** 与 query_v2 的 **block 迭代模型结构天然同构**——SNII 几乎是
为 block-wand 量身打造的。逐项对齐（SNII 侧出处见 `frq_prelude.h:25-67`、
`windowed_posting.h`）：

| query_v2 block 概念 | SNII windowed 对应物 | 出处 |
|---|---|---|
| 一个 block | 一个 window | `WindowMeta` |
| `skipToBlock(docid)` | `FrqPreludeReader::locate_window(docid,…)`（二级 skip 目录二分） | `frq_prelude.h:64` |
| `getLastDocInBlock()` | `WindowMeta::last_docid` | `frq_prelude.h:32` |
| `getMaxBlockFreq()` | `WindowMeta::max_freq` | `frq_prelude.h:58` |
| `getMaxBlockNorm()` | `WindowMeta::max_norm` | `frq_prelude.h:59` |
| `readBlock` 填 `doc_many`/`freq_many` | `decode_window_slices()` 解 dd/freq 区 | `windowed_posting.h:96-100` |
| `nextDeltaPosition` / 位置流 | `read_prx_window_csr()` 出 per-doc 位置 | `prx_pod.h` |
| docs-only 跳过 freq | windowed payload `[prelude][dd-block][freq-block]`，docs-only 只取 `[prelude][dd-block]`（dd-block 连续，1 个 GET） | `frq_prelude.h:13-23` |

**结论**：query_v2 的 block-wand 所需的 block-max 元数据（`getMaxBlockFreq/Norm`、
`getLastDocInBlock`、`skipToBlock`），SNII **已经在 prelude 里存好了**，且 prelude 是
**一次小读**即可拿到的二级 skip 目录。这意味着适配**不是**在 CLucene 的低层
`TermDocs`/`TermPositions` 上硬塞，而是**把 SNII window 直接当作 query_v2 的 block 源**。

---

## 3. 适配架构（What）

### 3.1 总体策略：复用算子，替换叶子，外加预取协调器

```
┌─────────────────────── query_v2 算子层（全部复用，零改动） ───────────────────────┐
│  BooleanQuery / IntersectionScorer(AndScorer) / DisjunctionScorer / UnionScorer /  │
│  BlockWand / PhraseScorer / PhrasePrefix / Prefix / Wildcard / Regexp / …          │
│  —— 仅依赖 DocSet / Scorer / Postings 抽象接口，不关心底层格式                       │
└───────────────────────────────────┬───────────────────────────────────────────────┘
                                     │ DocSet / Postings 接口
┌────────────────────────────────────▼──────────────────────────────────────────────┐
│  【新增】SniiSegmentPostings —— query_v2 Postings 的 SNII 原生实现（叶子替换）       │
│   - advance/seek/doc/freq/norm  ← SNII lazy-window 游标（prelude 先行，窗口按需物化）│
│   - append_positions_with_offset ← SNII prx CSR 解码                                 │
│   block 源 = SNII window；skipToBlock = locate_window；block-max = WindowMeta        │
└───────────────────┬─────────────────────────────────────────┬──────────────────────┘
                    │                                          │
┌────────────────────▼─────────────────┐   ┌───────────────────▼──────────────────────┐
│ 【新增】SniiPrefetchCoordinator       │   │ SNII reader / io（既有，零改动）          │
│  查询树构建期收集所有叶子 range，      │   │  LogicalIndexReader.lookup/resolve_*_window│
│  合并成 1 轮 BatchRangeFetcher         │──▶│  BatchRangeFetcher.add/fetch/get          │
│  保持「跨词一轮取」                    │   │  windowed_posting / FrqPreludeReader      │
└───────────────────────────────────────┘   └───────────────────────────────────────────┘
```

要点：
1. **算子层全部复用**：query_v2 的 boolean/intersection/union/wand/phrase 逻辑只依赖
   `DocSet`/`Scorer`/`Postings` 抽象（`scorer.h`、`union_postings.h`、`disjunction_scorer.h`、
   `intersection_scorer.h`、`wand/block_wand.h`）。它们**不关心底层是 CLucene 还是 SNII**。
2. **只替换叶子**：新增 `SniiSegmentPostings`（实现 query_v2 的 `Postings`/`DocSet` 接口），
   其 block 源是 **SNII 的惰性窗口游标**（移植 SNII 现有
   `scoring_query_wand_selective` 的 `BuildLazyWindowed` + `MaterializeWindow` 思路：
   prelude 先行，单窗口按需物化）。
3. **新增预取协调器**保持批量模型（§3.3）。

### 3.2 叶子绑定点（Seam）

query_v2 的叶子构造经由 `Weight::create_term_posting` /`create_position_posting`
（`weight.h`），二者最终调工厂模板
`make_term_doc_ptr` / `make_term_positions_ptr`
（`inverted_index_common.h:60,63`）。**适配在此切入**，两条可选路线：

- **路线 A（工厂特化）**：为 SNII reader 类型特化
  `make_term_doc_ptr` / `make_term_positions_ptr`，返回一个把 `lucene::index::TermDocs`
  接口（`readBlock`/`skipToBlock`/`nextDeltaPosition`…）实现到 SNII window 上的适配器。
  *优点*：query_v2 上层零改动；*缺点*：受限于 CLucene `DocRange`/`TermDocs` 的形状
  （尤其 `norm_many`，见 §4.1），且块拉取仍是「逐块」语义，需要预取层兜批量。
- **路线 B（原生 Postings，推荐）**：新增 `SniiSegmentPostings : public query_v2::Postings`，
  绕开 CLucene `TermDocs`，直接实现 `DocSet`（`advance/seek/doc/freq/norm`）与
  `append_positions_with_offset`。在 `Weight::scorer()` 构造叶子 scorer 时注入它，
  替换 `SegmentPostings`。*优点*：直接对接 SNII window 语义与 `BatchRangeFetcher`，
  无需迁就 `DocRange` 的内存形状；query_v2 的复合算子仍**原样复用**（它们只见
  `Postings`/`Scorer` 接口）。

**采纳路线 B**：它在「复用算子」与「保持 SNII 批量模型」之间最干净。路线 A 作为
最小改动回退（若 query_v2 上层对 `SegmentPostings` 具体类型有硬依赖，再退回 A）。

### 3.3 保持批量模型：预取协调器

惰性迭代器的批量化是本设计的难点。分两层解决：

**(a) 词内（per-term）一轮**：`SniiSegmentPostings` 构造时**只取 prelude**
（`fetch_windowed_prelude`，1 次小读，拿到全部 window 的 skip 目录 + block-max）。
随后窗口在 `readBlock`/`skipToBlock` 落入时**按需物化**，且**同词多窗口经
`BatchRangeFetcher(reader, kSameTermCoalesceGap)` 合并**（沿用 SNII 现有
`DecodeWindowedDocids` 的 16 KiB 邻接合并）。docs-only 路径下 dd-block 连续 → 整词
docids **1 个 GET**。

**(b) 跨词（cross-term）一轮**：新增 `SniiPrefetchCoordinator`。在
`Query::weight()`→`Weight::scorer()` **构造查询树时**做一次**规划遍历**：

1. 对所有叶子 term 调 `lookup`（BSBF 拒绝的词直接剔除，零字节）；
2. 收集每个叶子的**第一轮 range**（windowed→prelude；slim pod_ref→dd 区
   [+ phrase 的 prx]；inline→无 I/O）；
3. **一次** `BatchRangeFetcher::fetch()` 取齐 → 把结果切片回填各叶子
   `SniiSegmentPostings`。

这样「N 个词的首轮读」= **1 个串行轮次**，与 SNII 现有
`phrase_query` 的 `PlanTerms`+`round1.fetch()`（`phrase_query.cpp:444-451`）等价。
后续 conjunction 推进时再按需物化覆盖窗口（每词每轮合并，仍是 SNII 现行策略）。

> 关键不变量：**串行轮次 = 阶段数，而非 block 数 / term 数**。预取协调器把
> query_v2「按迭代触发 I/O」收敛回 SNII「按阶段触发 I/O」。

---

## 4. 接口落差与解决

### 4.1 `DocRange.norm_many`：norm 不在 posting 块里

query_v2 的块结构 `DocRange{doc_many, freq_many, norm_many, doc_many_size_}`
（`segment_postings.h`）要求**每 doc 的 norm 内联在块里**；而 SNII 把 norm 存在**独立的
`norms` 区**（1 字节/doc，`SectionRefs::norms`，`per_index_meta.h:43-68`），window 内
只存 `max_norm`（block-wand 用）。

- **路线 B 解法**：`SniiSegmentPostings::norm()` 直接走 SNII 的
  `stats.encoded_norm(docid)`（SNII BM25 现行做法，`bm25_scorer` / `scoring_query.cpp`）。
  norms 区很小（1 字节/doc），可在 segment 打开时**一次性预取常驻**，`norm()` 走内存。
- **路线 A 解法**：`readBlock` 填 `norm_many` 时，从已预取的 norms 区按 docid 抓字节。

### 4.2 位置 API：绑定在 `append_positions_with_offset` 而非 `nextDeltaPosition`

query_v2 短语实际消费位置的高层 API 是
`Postings::append_positions_with_offset(offset, out)`（`segment_postings.h:33-44`、
phrase scorer 填 vector），底层才是 `nextDeltaPosition`。SNII 的
`read_prx_window_csr()` 直接产出 **per-doc 位置列表**（CSR）。

- **绑定在高层**：`SniiSegmentPostings::append_positions_with_offset` 直接用 SNII CSR 结果填
  `out`（加 `offset`），**不必**模拟 `nextDeltaPosition` 的逐 delta 流。这样位置解码沿用
  SNII 现行**惰性**策略（只在短语验证阶段对幸存候选解 prx），规避此前 profiling 发现的
  「急切全量解位置」热点。
- 若 query_v2 短语逻辑硬依赖 `nextDeltaPosition`/`addLazySkipProxCount`，则在适配器内
  对当前 doc 的位置 vector 做一个轻量游标模拟（开销可忽略）。

### 4.3 三种 DictEntry 编码必须全部支持

SNII 词条有三条独立路径（不可合并，`dict_entry.h`、`format_constants.h:81-92`）：

| 编码 | 判定 | 读取 |
|---|---|---|
| **inline** | 极小 frq（≤256B） | 字节已在 `DictEntry.frq_bytes`/`prx_bytes`，**零 I/O** |
| **slim pod_ref** | df<512 且 frq>256B | `resolve_frq_window` → 1 次取 dd 区 |
| **windowed pod_ref** | df≥512 | prelude 先行 + 窗口按需物化 |

`SniiSegmentPostings` 须按 `entry.kind`/`entry.enc` 三路分派（SNII 现有
`term_query.cpp:72-96` 的分派可直接移植）。

### 4.4 `size_hint` / `cost`：基数估计

query_v2 用 `size_hint()`/`cost()`（`doc_set.h`、`size_hint.h`）做 AND 顺序规划。
SNII `DictEntry.df` 即 document frequency，直接作 `size_hint`。这让 query_v2 的
**低-df 优先驱动**与 SNII 现有 conjunction 的「按 df 升序」（`phrase_query.cpp:357-381`）
一致——保持「用最小词驱动、覆盖窗口最少」的字节优势。

---

## 5. 逐算子映射

| query_v2 算子 | 出处 | 需位置？ | SNII 读取映射 |
|---|---|---|---|
| TermQuery / TermScorer | `term_query/` | 否 | `lookup` → 三路分派；windowed 走 lazy-window 游标 |
| BooleanQuery（AND/OR/NOT） | `boolean_query/` | 否 | 叶子各自取；交并差在 `DocSet` 层组合，预取协调器保首轮 1 轮 |
| IntersectionScorer(AndScorer) | `intersection_scorer.h` | 否 | 按 df 升序，低-df 驱动，高-df 只物化覆盖窗口 |
| DisjunctionScorer / UnionScorer | `disjunction_scorer.h` / `buffered_union_scorer.h` | 否 | 堆归并多叶；各叶子 docs-only 1 轮取 |
| ExcludeScorer | `exclude_scorer.h` | 否 | include 叶 + exclude 叶，`DocSet` 层差集 |
| PhraseQuery / PhraseScorer | `phrase_query/` | **是** | 首轮取 prelude+dd[+prx]；conjunction 求交；幸存候选阶段惰性解 prx 验证 |
| PhrasePrefixQuery | `phrase_prefix_query/` | **是** | `prefix_terms` 枚举 + union + 短语验证 |
| PrefixQuery | `prefix_query/` | 否 | `LogicalIndexReader::prefix_terms`（`logical_index_reader.h:68`）枚举 → union |
| WildcardQuery / RegexpQuery | `wildcard_query/`、`regexp_query/` | 否 | term 枚举（前缀锚定）→ 过滤 → union |
| BlockWand | `wand/block_wand.h` | 否 | `getMaxBlockFreq/Norm`/`getLastDocInBlock`/`seek_block` ← prelude 的 `WindowMeta`，**无需取 .frq 块**即可剪枝 |
| AllQuery / MatchAllDocs | `all_query/` | 否 | 合成 scorer，**零 I/O**（与 SNII MATCH-ALL 一致） |
| BitSetQuery / ConstScoreQuery | `bit_set_query/`、`const_score_query/` | 否 | 位图/包装，无格式依赖 |

---

## 6. 三个黄金指标的保持（验收口径）

适配后**必须**用 `MeteredFileReader`（`metered_file_reader.h`，`IoMetrics`：
`serial_rounds`/`range_gets`/`remote_bytes`）对每个算子核验，目标对齐 SNII 现状：

1. **串行轮次 = 阶段数**：term=1；boolean/phrase 首轮=1，conjunction 每推进轮按需 1；
   block-wand 剪枝**不**额外增加轮次（block-max 在 prelude，首轮已取）。
2. **Range GET**：同词多窗口 16 KiB 邻接合并；docs-only dd-block 连续 → 1 GET。
3. **读取字节**：docs-only **不取 freq 块**（windowed payload 分区设计的核心收益）；
   短语只对幸存候选取/解 prx；block-wand 用 prelude block-max 跳窗，不取被剪窗口的 .frq。

> 回归基线：`bench --query-dir <persisted> --terms … --phrase …`（本仓库已有 local
> 成本模型模式，逐项打印三黄金指标 + read_at），适配前后**逐算子对比**，要求三指标
> 不劣于 SNII 现有算子，且 docids 与 query_v2/CLucene `ALL DOCIDS MATCH`。

---

## 7. 需要新增 / 修改的代码（合并侧）

| 文件 | 改动 | 说明 |
|---|---|---|
| `query_v2/segment_postings_snii.{h,cpp}`（新） | 新增 `SniiSegmentPostings : Postings` | 叶子替换；block 源=SNII lazy-window 游标；§3.2 路线 B |
| `query_v2/snii_prefetch_coordinator.{h,cpp}`（新） | 新增跨词首轮预取协调器 | §3.3(b)；保「N 词首轮=1 轮」 |
| `weight.h` / `term_weight.h` / `phrase_weight.h` | `scorer()` 注入 SNII 叶子 | 在 `create_term_posting`/`create_position_posting` 处分流到 SNII 实现 |
| `inverted_index_common.h:60,63` | （路线 A 时）特化工厂 | 仅当退回路线 A 才需要 |
| SNII `core/include/snii/reader/*` | **零改动**（只被调用） | `lookup`/`resolve_*_window`/`prefix_terms`/`windowed_posting` 已够用 |
| SNII `core/include/snii/io/batch_range_fetcher.h` | **零改动** | 适配器直接复用 |
| SNII `core/src/query/*` | **删除**（被 query_v2 取代） | 其读策略移植进 `SniiSegmentPostings`/协调器 |

> SNII reader 侧**不需要新增 API**——现有公共面
> （`lookup`/`resolve_frq_window`/`resolve_prx_window`/`section_refs`/`prefix_terms`
> + `BatchRangeFetcher` + `windowed_posting` 自由函数）已覆盖 query_v2 全部算子所需。

---

## 8. 风险与权衡

| # | 风险 | 缓解 |
|---|---|---|
| R1 | 朴素适配把惰性 `readBlock` 直连 `read_at` → 串行轮次爆炸 | 预取协调器（§3.3）把 I/O 收敛回「按阶段」；验收用 `IoMetrics` 守门（§6） |
| R2 | query_v2 上层硬依赖 `SegmentPostings` 具体类型，路线 B 注入受阻 | 回退路线 A（工厂特化），代价是迁就 `DocRange`（norm_many/块形状） |
| R3 | `norm_many` 与 SNII 独立 norms 区不匹配 | norms 区 1 字节/doc，segment 打开时常驻；`norm()` 走 `stats.encoded_norm`（§4.1） |
| R4 | 位置 API 落差（`nextDeltaPosition` vs CSR） | 绑定高层 `append_positions_with_offset`，保惰性解位置（§4.2） |
| R5 | block-wand 的 `seek_block`/block-max 语义与 SNII window 边界不一致 | SNII window 即 block，prelude 已存 `last_docid`/`max_freq`/`max_norm`（§2），一一对应 |
| R6 | 三种 DictEntry 编码遗漏某条路径 | `SniiSegmentPostings` 强制三路分派 + 三路各自的 round-trip 回归（移植 SNII 现有覆盖） |

---

## 9. 实施阶段建议（Phased）

1. **P1 叶子（docs-only）**：`SniiSegmentPostings` 实现 `DocSet`（三路分派，windowed
   lazy-window），跑通 TermQuery + BooleanQuery(AND/OR/NOT)，`IoMetrics` 对齐
   SNII `term_query`/`boolean_and`。
2. **P2 预取协调器**：跨词首轮 1 轮取，验证 boolean 多词串行轮次 = 阶段数。
3. **P3 位置**：`append_positions_with_offset` + PhraseQuery / PhrasePrefix，惰性解 prx，
   对齐 SNII `phrase_query` 三黄金指标。
4. **P4 评分/剪枝**：norm 接入 + BlockWand（prelude block-max），对齐 SNII WAND/selective。
5. **P5 枚举类**：Prefix / Wildcard / Regexp 走 `prefix_terms` + union。
6. **P6 收口**：删除 SNII `core/src/query/*`，全算子 `MeteredFileReader` 回归 +
   `ALL DOCIDS MATCH`。

---

## 10. 结论（一句话）

**复用 query_v2 的全部复合算子与评分逻辑，仅把叶子 `SegmentPostings` 换成 SNII 原生实现
（SNII window ≡ query_v2 block，prelude 即 skip 目录 + block-max），并加一个查询树级预取
协调器把惰性迭代触发的 I/O 收敛回 SNII「按阶段一轮并发取」——即可让 query_v2 读 SNII 格式
而三个黄金指标不退化。**
