# SNII 子块跳读（super-block / windowed sub-block skipping）设计

- 状态：设计待评审（实现前）
- 日期：2026-06-18
- 关联：扩展 [`2026-06-17-snii-design.md`](2026-06-17-snii-design.md)；源规格 [`../../design/SNII-design-spec.source.md`](../../design/SNII-design-spec.source.md)
- 动机：基准 [`../../benchmark-results.md`](../../benchmark-results.md) 5M 真实 OSS 实测发现的短板

## 1. 问题与目标

5M 真实 OSS 实测：5-term `MATCH_PHRASE` 含 df=4.65M 超高频词时，**SNII 真实 wall-clock 1260ms 反慢于 CLucene 960ms**，尽管 SNII 串行轮次仅 5（CLucene 21）。根因：**SNII 当前 `phrase_query` 读取该词整段 windowed 倒排表（~25MB）再做位置交集**，而 CLucene 用 skip-list 跳过高频词绝大部分倒排、只读相关段。规模越大、高频词倒排越巨，**字节传输/解码主导延迟**，轮次节省被字节量盖过。

**目标**：实现源规格设计的 **prelude 子块（窗口）跳读** + **super-block 粗目录**，使短语/位置查询只读取与候选 doc 相关的 `.frq/.prx` 子 range，把高频词短语的读取字节数从「整段」降到「命中窗口」，在保持低串行轮次的同时消除字节短板。

## 2. 源规格依据

源规格已设计该机制（不是新增功能，是补齐 v1 未实现的部分）：
- §窗口元信息prelude（line 502-536）：prelude 列 `C last_docid_delta[N]`、`D frq_window_len[N]`、`E prx_cum_off[M]`、`SB column: optional super-block directory`；「通过 C / D / E 列定位 `.frq` 和 `.prx` 窗口，生成可合并、可并发的 range」。
- §prx 设计对比（line 545-550）：「prelude 暴露 window / sub-block 的 docid 范围、offset、length、max score 等信息，然后决定哪些 `.frq/.prx` 子 range 要加入 BatchRangeFetcher」。
- DictEntry `flags.has_sb`（line 395）：「posting prelude 中带子块目录，可进一步拆分大 postings 的读取范围」。

## 3. 当前实现差距

- `logical_index_writer`：windowed term 只产 **1 个窗口**（`BuildPrelude` 写 N=1）。N=1 时无可跳读。
- `frq_prelude` 列：有 `last_docid_delta(C)`、`frq_window_len(D，per-window doc 数)`、`prx_cum_off(E，per-window .prx 字节累积偏移)`、`max_freq/max_norm`、`win_crc32c`；**缺 `.frq` 的 per-window 字节偏移列**（仅 .prx 有 E）。
- `phrase_query`：对每个 term 读整段 `[prelude][window]`（frq_len 字节）全解码再交集，不用 prelude 选窗口。

## 4. 格式：frq_prelude（一次定型，含两级子块跳读）

> SNII 从零开始、无前向兼容负担：prelude 直接就是支持子块跳读的正确格式，不做版本迭代。
> 现有的单窗口 prelude 实现直接替换为下面的两级（super-block → window）可跳读结构。

prelude 含一个 **super-block 粗目录** + 每个 super-block 自包含的 **window 目录块**，使 reader 两级二分跳读、且无需读完整 prelude：

```
prelude:
  header:
    u8   flags(bit0 has_freq, bit1 has_prx)
    VInt N             # .frq 窗口总数（windowed term；slim/inline 无 prelude）
    VInt G             # 每 super-block 的窗口数（如 64）
    VInt n_super       # = ceil(N / G)
    VInt sbdir_len     # super-block 目录字节长（使 reader 可只读目录）
    u32  crc32c        # 覆盖 header + super-block 目录（window 块各自带 win_crc）

  super_block_dir[n_super]:        # 小而常驻：每 super-block 一行
    VInt sb_last_docid_delta       # 该 super-block 末窗口末 docid 的 delta（累积=绝对）
    VInt sb_block_off              # 该 super-block 的 window 目录块在 prelude 内的字节偏移
    VInt sb_block_len

  window_dir (n_super 个自包含块，每块 ≤G 个窗口的行)：
    per window w (行式，自包含，便于按块独立读取)：
      VInt last_docid_delta        # 本窗口末 docid 的 delta（块内累积=绝对；前一窗口末 docid=win_base）
      VInt doc_count               # 窗口 doc 数（frq_pod 解码需要）
      VInt frq_off                 # 本窗口 .frq payload 在 .frq region 内的字节偏移（相对 window_start=frq_off+prelude_len）
      VInt frq_len
      VInt prx_off                 # 本窗口 .prx payload 字节偏移（has_prx）
      VInt prx_len
      VInt max_freq                # 窗口内最大 tf（WAND block-max）
      u8   max_norm                # 窗口内 score-max 对应 norm（WAND）
      u32  win_crc32c
```

**为何行式而非纯列式**：源规格提"列式 prelude"，但子块跳读要求按窗口/super-block 独立定位字节，行式 + super-block 分块更直接（reader 只读目录 + 命中块 + 命中窗口）。WAND 的 `max_freq` 仍可按行扫描，能力等价。

`N ≤ G` 时只有 1 个 super-block（小 term 退化为单块，读全 prelude，无额外开销）。DictEntry `flags.enc=windowed`，`has_sb` 恒置位（prelude 即两级目录），`prelude_len` 覆盖整个 prelude。

## 5. 写入改动：多窗口 + prelude/SB（logical_index_writer）

windowed term（df ≥ kSlimDfThreshold=512）的 posting 按 `kFrqBaseUnit`(256) 切成多窗口：
1. 将 docids/freqs/positions 按每窗口 256 doc 分片（最后一窗口可不足 256）。
2. 每窗口：`build_frq_window(win_docids, win_freqs, win_base, has_freq, auto_zstd)`，`win_base` = 前一窗口末 docid（首窗 0）；`build_prx_window(win_positions, ...)`。
3. 累积每窗口列：`last_docid_delta`（末 docid delta）、`frq_doc_count`、`frq_cum_off`（前缀和窗口字节长）、`prx_cum_off`、`max_freq`（窗口内最大 tf）、`max_norm`、`win_crc32c`。
4. N > 阈值时构建 SB 目录，置 DictEntry `has_sb`。
5. `build_frq_prelude(cols)` → prelude；`.frq` payload = `[prelude][win0][win1]...`；DictEntry `prelude_len`=prelude 字节、`frq_off/frq_len` 覆盖整体。`.prx` payload = `[win0][win1]...`（prx_off/prx_len 覆盖）。

复用既有 `build_frq_window`/`build_prx_window`/`build_frq_prelude`（仅 prelude 列扩充 + 多窗口循环）；merge byte-copy 等高级特性不在本期。

## 6. 读取改动

### 6.1 frq_prelude reader（两级）
- `open(prelude_slice)`：解析 header + super-block 目录（小、常驻），校验 crc；window 行块按需解析。
- `locate_super_block(docid)`：在 super_block_dir 的 `sb_last_docid` 上二分 → super-block 序号 + 其 window 块的字节范围。
- `WindowMeta window(w)`：返回 last_docid / win_base（前窗末 docid）/ doc_count / frq_off / frq_len / prx_off / prx_len / max_freq / max_norm。
- `locate_window(docid)`：两级——先 super-block 二分，再块内 window 二分 → 覆盖该 docid 的窗口序号。
- n_super 小且常驻时可一次解析全部窗口；超大 term 仅按需解析命中 super-block 的 window 块。

### 6.2 phrase_query（核心改动：两级跳读）
现状「读全段 → 全解码 → 交集」改为「读 prelude → 候选驱动两级定位窗口 → 批量取命中窗口 → 交集」：
1. lookup 各 term → DictEntry + frq_base/prx_base。inline/slim term 照旧（小，直接解码）。
2. 第 1 轮：批量读各 windowed term 的 **prelude header + super-block 目录**（小）。
3. 选 **lead term = df 最小者**；读其全部窗口（小）得候选 docid 序列。
4. 对其余 term：对每个候选 docid 两级定位（super-block 二分 → window 二分）得覆盖窗口集合（去重）；按需读取命中 super-block 的 window 块以拿到这些窗口的 `frq_off/len`、`prx_off/len`。第 2 轮 `BatchRangeFetcher` 批量并发取这些窗口的 `.frq`(+`.prx`) 子 range。
5. 解码命中窗口，取候选 doc 的 freq/positions，做位置邻接交集（沿用现有 `PhraseInDoc`）。

效果：超高频词只读「覆盖候选 doc 的窗口」而非整段 → 字节数从 O(df) 降到 O(候选数×窗口大小)；super-block 目录使连完整 prelude 都不必读。轮次 ≈ 2–3（目录 + 命中 window 块 + 命中窗口，各批量），保持低轮次。

### 6.3 term_query（不变）
纯 term 枚举需全部 docid → 仍读全部窗口（批量并发，字节与现状相同）。多窗口不劣化（现 5M term SNII 已快 5.1×）。

### 6.4 BM25 WAND（受益）
现 WAND 用 prelude `max_freq/max_norm` 做窗口级 block-max；但当前 N=1 无可剪。多窗口后 WAND 真正按窗口剪枝，scoring 也少读字节。

## 7. 正确性与测试

- **不变量**：跳读结果必须与「读全段交集」逐 docid 相等。新增差分测试：构造含 df≥512（多窗口，≥若干窗口）的词 + 短语，断言 `phrase_query`（跳读）docid 集合 == 朴素 oracle == 旧全读路径（可用一个 force-full 开关或独立参考）。
- 现有 e2e/oracle、WAND 差分测试（df=700→现多窗口）必须仍绿。
- 边界：候选 doc 落在窗口边界、跨多窗口、lead term 也是 windowed、单窗口 term（df 略≥512 但 ≤256 不可能，故 df≥512 必 ≥2 窗口）、空候选。
- 损坏：Foff/SB 越界、非单调 → Corruption（沿用既有 anti-DoS 模式）。
- 基准：5M `--oss` 短语预期从 SNII 1260ms 降到与 1M 同量级（只读命中窗口），反超 CLucene。

## 8. 实现分期（每期闭环、TDD、对抗审查）

1. **格式**：frq_prelude 重写为两级（super-block 目录 + window 行块）可跳读结构（替换现单窗口实现）+ 访问器（`super_block(s)`、`window(w)`：last_docid/win_base/doc_count/frq_off/frq_len/prx_off/prx_len/max_freq/max_norm）+ round-trip/损坏/越界测试。
2. **写入**：logical_index_writer 多窗口（256-doc unit）+ super-block 分组 + prelude 构建（read-back 自校验：用 prelude 定位每窗口并解码一致）。
3. **读取**：phrase_query 两级跳读（super-block 二分 → window 二分 → 批量取命中窗口）；差分测试 vs 全读参考/oracle，含多窗口超高频词短语。BM25 WAND 现按真实多窗口剪枝。
4. **验证**：全量测试全绿 + 5M `--oss` 重测短语（预期反超 CLucene），更新基准报告。

一次实现完整的两级跳读（super-block + window），不分步骤交付半成品。
