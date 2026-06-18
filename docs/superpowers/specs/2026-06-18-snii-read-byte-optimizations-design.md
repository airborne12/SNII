# SNII 读取字节数优化设计（#1–#5）

- 状态：已实现并实测（commits 至 49，238 core 测试全绿）
- 日期：2026-06-18
- 关联：扩展 [`2026-06-18-snii-subblock-skipping-design.md`](2026-06-18-snii-subblock-skipping-design.md)；源规格 [`../../design/SNII-design-spec.source.md`](../../design/SNII-design-spec.source.md) §倒排表设计 / §Scoring
- 依据：设计覆盖审计（111 设计点，~92% 已实现）找出的剩余「读取字节数」优化项
- 原则：从零、无前向兼容、一把做对（同子块跳读设计）

## 0. 背景

设计文档三大评估指标之一是**读取字节数**（line 47-56）。审计发现核心已实现，但 `dd_part_len`（源规格 line 496/501：bitmap/docid-only 跳过 freq）等省字节设计当时只在内存 CPU 跳过、未在网络上跳过：docid-only term 与所有短语仍把每窗 freq_part（占 30-60% 字节）整段拉下来再丢弃。本设计覆盖 #1–#5。优先级遵循源规格 line 86：**先降串行轮次与远端请求数，其次才是字节数**。

**为什么 freq-skip 必须做到 posting 级**（关键设计约束）：要让 docid-only 不取 freq，dd 数据必须能独立 fetch+decode。仅在「每窗内」分离 dd/freq（`[win0=dd0+fq0][win1=dd1+fq1]…`）不够——posting 里 dd 与 freq 仍交错，docid-only 必须逐窗取 dd 前缀，导致两个问题：① **read_at 碎片化**（高频 term 一窗一读 → 数千次逻辑读，虽批量进同样轮次，但违反设计「降 read_at」初衷）；② **1MB FileCache 下不省字节**（dd 与 freq 共享同一 1MB 块，块对齐后跳 freq 等于没跳）。因此 dd/freq 必须分离到 **posting 级**，使 docid-only 读**一段连续 dd 块**——三个指标（read_at / range_gets / 字节）同时降，FileCache 与直连 S3 下都省。

## 1. #1 freq-skip：posting 级 dd/freq 分组（最大字节优化）

### 1.1 格式：`[prelude][dd-block][freq-block]`
windowed term 的 `.frq` payload 把所有窗口的 dd 区连续排在前、freq 区连续排在后；窗口本身不再自描述，编解码元数据全部上提到 prelude：
```
windowed .frq payload = [prelude][dd-block][freq-block]
  dd-block   = dd_region_0 ++ dd_region_1 ++ … ++ dd_region_{N-1}   # 各窗 dd 区连续，各自 raw/zstd
  freq-block = freq_region_0 ++ … ++ freq_region_{N-1}             # has_freq 时；各窗 freq 区连续
  dd_region  = raw 或 zstd( VInt n ++ PFOR_runs(doc_delta) )        # 按区大小自适应 raw/zstd
  freq_region= raw 或 zstd( PFOR_runs(freq) )
```
dd/freq 各区独立编码（各自 raw/zstd），各带 crc，可单独取回校验。`frq_docs_len = prelude_len + dd_block_len`（docs-only 前缀 = `[prelude][dd-block]`，连续可一段读取）。

### 1.2 prelude 携带每窗完整定位/编解码元数据
frq_prelude 每窗行（在两级 super-block/window 目录之上）携带：
```
last_docid, win_base, doc_count, max_freq, max_norm,
dd_off(在 dd-block 内偏移), dd_disk_len, dd_uncomp_len, win_mode_dd, crc_dd,
freq_off(在 freq-block 内偏移), freq_disk_len, freq_uncomp_len, win_mode_freq, crc_freq
```
prelude reader 暴露 `dd_block_len()` / `freq_block_len()`，并校验各 dd 区在 dd-block 内连续平铺、各 freq 区在 freq-block 内连续平铺（拒绝缝隙/越界/长度不一致）。slim DictEntry 的单窗 `[dd_region][freq_region]` 同样记 dd/freq region 元数据，`frq_docs_len = dd 区落盘长度`；inline 字节已内嵌、不走网络、无需改。

### 1.3 写入
`build_dd_region` / `build_freq_region` 分别编码每窗 dd、freq 区（各自 `should_compress`）。`BuildWindowedPosting` 把所有 dd 区拼成 dd-block、所有 freq 区拼成 freq-block，逐窗填 prelude 行（offset/len/mode/crc）；`build_windowed_entry` 落盘 `[prelude][dd-block][freq-block]`，置 `frq_docs_len = prelude_len + dd_block_len`。

### 1.4 读取（按需取 freq）
- `term_query`（**docid-only**）：读 `[frq_off, frq_off+prelude_len+dd_block_len)` —— **一段连续 dd 块**，按 prelude 的 (dd_off, dd_disk_len, win_mode_dd, crc_dd) 逐窗解码。**不取 freq-block**。
- `phrase_query`：读 dd-block（docid）+ `.prx`（positions），跳 freq-block（短语不需 freq）；子块跳读时只取命中窗口的 dd 子段（coalesce 合并）。
- `scoring_query`：额外读连续 freq-block，按 (freq_off, freq_disk_len, crc_freq) 解 freq；选择性 WAND（#5）只取存活窗口的 dd+freq 子段。
- 解码入口：`decode_dd_region`（从 dd 区切片解 docids）/ `decode_freq_region`（从 freq 区切片解 freqs）。

### 1.5 收益（已实测）
docid-only term 与所有短语读**一段连续 dd 块**，跳过整个 freq-block：
- read_at 不再碎片化（高频 term 连续读，read_at 小）；range_gets 降；
- 字节降且 **FileCache 下也省块**（freq-block 的块完全不触及）。

5M 实测（CLucene→SNII，`ALL DOCIDS MATCH`）：TERM high-df read_at 43→3、remote_bytes 4.2→3.1MB、request_bytes 2.82→1.37MB(2×)；PHRASE request_bytes 10.77→0.96MB(**11×**)。真实 OSS（`--oss --repeat`）：TERM high-df 中位 50ms vs CLucene 427ms(8.5×)、PHRASE 107ms vs 667ms(6.2×)。

## 2. #2 同-term 多窗口 coalesce_gap（降 Range GET 数）

`BatchRangeFetcher` 已支持 `coalesce_gap`，但所有调用点用默认 0（仅严格相邻才合并）。同一 term 的多个选中窗口在 .frq POD 内近乎连续；取同一 term 多窗口时传入小 `coalesce_gap`（如 16KB），让近邻窗口并成一个物理 GET。以极小过读换更少 Range GET（设计更高优先级指标 line 86）。改动：term/phrase/scoring 多窗口 fetch 处构造 `BatchRangeFetcher(reader, kSameTermCoalesceGap)`。

## 3. #3 真实 max_norm（紧 WAND 边界）

现 `logical_index_writer` 把每窗 `max_norm=0`（最宽松），WAND 上界偏松。改为：`BuildWindowedPosting` 对每窗取 **使 BM25 长度因子最大的 encoded_norm**（即长度惩罚最小者）存为 `max_norm`。reader 已消费该字段。无直接字节变化，但是 #5 选择性读窗口的前提，并改善 WAND CPU 剪枝。

## 4. #4 自适应窗口 512/1024/2048（降常驻 prelude + 头开销）

现 `BuildWindowedPosting` 固定 256-doc 切窗；高频词（df=5M→~2 万窗）的 prelude 行与窗口头/crc 开销巨大。改为按 df 阈值组合 unit：低 df windowed 用 256，高 df 用 1024/2048（仍以 256-doc unit 为基准对齐 .prx）。减少 prelude 行数 ~4-8×（常驻元数据更小）+ 每窗头/crc 开销，PFOR run 更长压缩略好。需与 #1 平衡：窗口越大、freq-skip 与跳窗粒度越粗。

## 5. #5 WAND 选择性读窗口（scoring top-K 只读存活窗口）

现 `BuildWindowedCursor`（scoring）经 `read_windowed_posting` 读**全部窗口**，WAND 只跳「打分」不跳「读取」。改为两遍：① 只读各 windowed term 的 prelude（block-max 列）；② 用 block-max 上界 + 运行中 top-K 阈值 θ，仅 batch-fetch `max_score ≥ θ` 可能进 top-K 的窗口（复用 phrase 的 `windowed_window_range`/`decode_window_slices`）。top-K 读 O(存活窗口) 而非 O(df)。依赖 #3 真实 max_norm 才有效。

## 6. 实现分期（每期 TDD + 对抗审查 + bench 实测字节，均已落地）

- **格式 + 写入**：区级 frq_pod（`build_dd_region`/`build_freq_region`/`decode_dd_region`/`decode_freq_region`）+ prelude 每窗携带 dd/freq region 元数据 + slim DictEntry region 元数据；posting 落盘 `[prelude][dd-block][freq-block]`；#3 真实 max_norm + #4 自适应窗口（1024-doc，df≥8192）。read-back 自校验：docs-only 连续 dd 块独立解出 docids 且与全窗一致。
- **读端字节跳过**：term docid-only + phrase 读连续 dd 块、跳 freq-block + #2 同-term coalesce_gap。差分测试：term/phrase docid 与全读/oracle 一致；断言 read_at / range_gets / request_bytes 三降。
- **选择性 scoring（#5）**：`scoring_query_wand_selective` 仅取 block-max 可入 top-K 的窗口。差分测试：top-K 与 exhaustive/eager 完全一致（含并列、k=1），且小-k 高频读更少窗口/字节。
- **验证**：238 测试全绿；成本模型轨 100K/1M/5M + 真实 OSS `--oss --repeat` 实测（见 `../../benchmark-results.md`）；bench 增 `request_bytes`（精确字节）区分 FileCache 块对齐。

## 7. 正确性不变量

- docs-only 解码结果 == 全窗解码的 docids（逐 docid）。
- 短语/term 跳 freq 后 docid 集合 == oracle == 全读路径。
- #5 selective WAND top-K（docid+score）== exhaustive top-K，含并列（沿用 `>=` θ 既有修复与差分测试）。
- 损坏/越界：两段 crc、dd_disk_len/freq_disk_len 越界、frq_docs_len > frq_len → Corruption（沿用 anti-DoS）。
- 兼容：windowed/slim 格式变化 → 重建索引（从零、无兼容负担）。
