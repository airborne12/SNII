# SNII 读取字节数优化设计（#1–#5）

- 状态：设计待评审（实现前）
- 日期：2026-06-18
- 关联：扩展 [`2026-06-18-snii-subblock-skipping-design.md`](2026-06-18-snii-subblock-skipping-design.md)；源规格 [`../../design/SNII-design-spec.source.md`](../../design/SNII-design-spec.source.md) §倒排表设计 / §Scoring
- 依据：设计覆盖审计（111 设计点，~92% 已实现）找出的剩余「读取字节数」优化项
- 原则：从零、无前向兼容、一把做对（同子块跳读设计）

## 0. 背景

设计文档三大评估指标之一是**读取字节数**（line 47-56）。审计发现：核心已实现，但仍有 5 项已设计/可做的字节与请求优化未落地。本设计覆盖全部 5 项。优先级遵循源规格 line 86：**先降串行轮次与远端请求数，其次才是字节数**——故 #2（降 GET 数）与 #1（降字节）并重。

当前 frq 窗口落盘布局（`frq_pod.h`）：
```
window: u8 win_mode(0=raw/1=zstd); VInt uncomp_len; [VInt comp_len if zstd];
        VInt dd_part_len; u32 crc; payload(raw 或 zstd 整体压缩) = dd_part ++ freq_part
  dd_part = VInt n ++ PFOR_runs(doc_delta);  freq_part = PFOR_runs(freq)
```
问题：zstd 窗口把 `dd_part ++ freq_part` **整体压成一个 blob**，无法只取 dd 前缀解压；且 `dd_part_len` 藏在 header 内（取回整窗后才知道）。故 docid-only / 短语查询当前把每窗 freq_part（占 30-60% 字节）整段拉下来再内存丢弃。

## 1. #1 dd_part_len 网络级 freq-skip（最大字节优化）

### 1.1 格式：窗口 dd/freq 区独立编码
窗口 payload 改为 **两个独立编码区**，各自 raw/zstd（按区大小自适应），header 记录 dd 区落盘长度，使 `[header + dd_region]` 成为可独立 fetch+decode 的前缀：
```
window:
  u8   win_mode_dd     # dd 区编码：0=raw / 1=zstd
  u8   win_mode_freq   # freq 区编码（has_freq 时）
  VInt dd_uncomp_len   # dd 区解压字节
  VInt dd_disk_len     # dd 区落盘字节（=raw 时==uncomp）
  VInt freq_uncomp_len # has_freq 时
  VInt freq_disk_len   # has_freq 时
  u32  crc_dd          # 覆盖 header + dd 区落盘字节（使 docs-only 取回即可独立校验）
  bytes dd_region      # raw 或 zstd(dd_part)
  [u32  crc_freq        # 覆盖 freq 区落盘字节]
  [bytes freq_region]   # raw 或 zstd(freq_part)
```
`frq_docs_len` = `[header + crc_dd + dd_region]` 的总落盘字节（docid-only 前缀长度）。整窗 `frq_len` 不变（含 freq 区）。

### 1.2 prelude / DictEntry 暴露 docs 前缀长度
- frq_prelude window 行新增 `frq_docs_len`（每窗 docs-only 前缀落盘字节）。
- slim DictEntry 新增 `frq_docs_len`（单窗 slim 的 docs-only 前缀）。inline 形态字节已内嵌、不走网络、无需改。

### 1.3 写入
`build_frq_window` 分别编码 dd 区、freq 区（各自 `should_compress`），写两段 header 列 + 两段 crc + 两区；返回/记录 `frq_docs_len`。writer 把每窗 `frq_docs_len` 填入 prelude 行 / slim DictEntry。

### 1.4 读取（按需取 freq）
- `term_query`（**docid-only**）：每窗只 fetch `[frq_off, frq_off+frq_docs_len)`，用 `read_frq_window_docs` 解 dd 区。**不取 freq 区**。
- `phrase_query`：.frq 每窗只 fetch `frq_docs_len`（短语只需 docid+positions，不需 freq）；.prx 照常全取。
- `scoring_query`：fetch 整窗 `frq_len`（需要 freq 打分）——不变。
- 新增解码入口：`read_frq_window_docs` 从 `[header+dd_region]` 解 docids（freq 区缺失时正确处理）；保留 `read_frq_window`（docs+freq）供 scoring。

### 1.5 收益
filter（docid-only term）与**所有短语**的 .frq 字节 **−30~50%**（freq 区不过网）。直接命中「读取字节数」指标。

## 2. #2 同-term 多窗口 coalesce_gap（降 Range GET 数）

`BatchRangeFetcher` 已支持 `coalesce_gap`，但所有调用点用默认 0（仅严格相邻才合并）。同一 term 的多个选中窗口在 .frq POD 内近乎连续；取同一 term 多窗口时传入小 `coalesce_gap`（如 16KB），让近邻窗口并成一个物理 GET。以极小过读换更少 Range GET（设计更高优先级指标 line 86）。改动：term/phrase/scoring 多窗口 fetch 处构造 `BatchRangeFetcher(reader, kSameTermCoalesceGap)`。

## 3. #3 真实 max_norm（紧 WAND 边界）

现 `logical_index_writer` 把每窗 `max_norm=0`（最宽松），WAND 上界偏松。改为：`BuildWindowedPosting` 对每窗取 **使 BM25 长度因子最大的 encoded_norm**（即长度惩罚最小者）存为 `max_norm`。reader 已消费该字段。无直接字节变化，但是 #5 选择性读窗口的前提，并改善 WAND CPU 剪枝。

## 4. #4 自适应窗口 512/1024/2048（降常驻 prelude + 头开销）

现 `BuildWindowedPosting` 固定 256-doc 切窗；高频词（df=5M→~2 万窗）的 prelude 行与窗口头/crc 开销巨大。改为按 df 阈值组合 unit：低 df windowed 用 256，高 df 用 1024/2048（仍以 256-doc unit 为基准对齐 .prx）。减少 prelude 行数 ~4-8×（常驻元数据更小）+ 每窗头/crc 开销，PFOR run 更长压缩略好。需与 #1 平衡：窗口越大、freq-skip 与跳窗粒度越粗。

## 5. #5 WAND 选择性读窗口（scoring top-K 只读存活窗口）

现 `BuildWindowedCursor`（scoring）经 `read_windowed_posting` 读**全部窗口**，WAND 只跳「打分」不跳「读取」。改为两遍：① 只读各 windowed term 的 prelude（block-max 列）；② 用 block-max 上界 + 运行中 top-K 阈值 θ，仅 batch-fetch `max_score ≥ θ` 可能进 top-K 的窗口（复用 phrase 的 `windowed_window_range`/`decode_window_slices`）。top-K 读 O(存活窗口) 而非 O(df)。依赖 #3 真实 max_norm 才有效。

## 6. 实现分期（每期 TDD + 对抗审查 + bench 实测字节）

- **Phase A（格式 + 写入）**：#1 窗口 dd/freq 独立编码 + prelude `frq_docs_len` 列 + slim `frq_docs_len` + #3 真实 max_norm + #4 自适应窗口。read-back 自校验：docs-only 前缀可独立解出 docids 且与全窗一致；max_norm 非 0 且为窗内最紧。全量测试绿。
- **Phase B（读端字节跳过）**：#1 reader（term docid-only + phrase 只取 docs 前缀）+ #2 coalesce_gap。差分测试：term/phrase docid 与全读/oracle 一致；**断言 remote_bytes 显著下降**（短语 .frq 字节较跳读前再降）。
- **Phase C（选择性 scoring）**：#5。差分测试：scoring top-K 与全读 exhaustive 完全一致（含并列）；断言只读存活窗口（range_gets/bytes < 全窗）。
- **验证**：全量测试 + `snii_bench` 成本模型轨重测，确认 PHRASE / TERM 的 remote_bytes 下降、docids 仍三方匹配；OSS 轨可选实测。

## 7. 正确性不变量

- docs-only 解码结果 == 全窗解码的 docids（逐 docid）。
- 短语/term 跳 freq 后 docid 集合 == oracle == 全读路径。
- #5 selective WAND top-K（docid+score）== exhaustive top-K，含并列（沿用 `>=` θ 既有修复与差分测试）。
- 损坏/越界：两段 crc、dd_disk_len/freq_disk_len 越界、frq_docs_len > frq_len → Corruption（沿用 anti-DoS）。
- 兼容：windowed/slim 格式变化 → 重建索引（从零、无兼容负担）。
