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

## 4. 格式改动：frq_prelude v2

提升 `kFrqPreludeVersion` 1→2，**新增 `.frq` per-window 字节偏移列**（对称于 E 的 `prx_cum_off`），并定义可选 super-block 目录：

```
prelude (v2):
  header:
    u8   ver (=2)
    u8   flags(bit0 has_freq, bit1 has_prx, bit2 has_sb)
    VInt N            # .frq 窗口数
    VInt M            # .prx 窗口数 (has_prx)
    VInt G            # super-block 分组大小（窗口/super-block；has_sb 时存在）
    VInt col_len[]
    u32  crc32c
  columns:
    B   max_freq[N]            (varint32)   # 既有
    B2  max_norm[N]            (u8)         # 既有
    C   last_docid_delta[N]    (varint32)   # 既有：每窗口末 docid 的 delta（累积=绝对末 docid；前一窗口末 docid = 本窗口 win_base）
    D   frq_doc_count[N]       (varint32)   # 既有 frq_window_len，语义=每窗口 doc 数（解码用）
    Foff frq_cum_off[N+1]      (varint64)   # 新增：每窗口 .frq 字节累积偏移（相对 window_start=frq_off+prelude_len）；窗口 w 范围=[Foff[w],Foff[w+1])
    E   prx_cum_off[M+1]       (varint64)   # 既有（扩为 N+1/M+1 含尾，使最后一窗口范围可定）
    H   win_crc32c[N]          (fixed32)    # 既有
    SB  super-block 目录        (has_sb)     # 新增（见下）
```

**Super-block 目录（SB，可选，超大 term）**：当 N 超过 `kSuperBlockThreshold`（如 1024 窗口）时，按每 G 窗口分组（如 G=64），记录每个 super-block 的粗信息，使 reader 不必读完整 prelude：

```
SB:
  VInt n_super   # = ceil(N / G)
  per super-block s:
    VInt last_docid_delta_s   # super-block s 末 docid 的 delta（累积=绝对）
    VInt frq_off_s            # super-block s 首窗口的 .frq 字节偏移（= Foff[s*G]）
    VInt prx_off_s            # 同 .prx
```

兼容：v2 prelude 与 v1 不互通；windowed term 格式变化 → 需重建索引（v1 库可接受）。slim/inline term 不受影响。

## 5. 写入改动：多窗口 + prelude/SB（logical_index_writer）

windowed term（df ≥ kSlimDfThreshold=512）的 posting 按 `kFrqBaseUnit`(256) 切成多窗口：
1. 将 docids/freqs/positions 按每窗口 256 doc 分片（最后一窗口可不足 256）。
2. 每窗口：`build_frq_window(win_docids, win_freqs, win_base, has_freq, auto_zstd)`，`win_base` = 前一窗口末 docid（首窗 0）；`build_prx_window(win_positions, ...)`。
3. 累积每窗口列：`last_docid_delta`（末 docid delta）、`frq_doc_count`、`frq_cum_off`（前缀和窗口字节长）、`prx_cum_off`、`max_freq`（窗口内最大 tf）、`max_norm`、`win_crc32c`。
4. N > 阈值时构建 SB 目录，置 DictEntry `has_sb`。
5. `build_frq_prelude(cols)` → prelude；`.frq` payload = `[prelude][win0][win1]...`；DictEntry `prelude_len`=prelude 字节、`frq_off/frq_len` 覆盖整体。`.prx` payload = `[win0][win1]...`（prx_off/prx_len 覆盖）。

复用既有 `build_frq_window`/`build_prx_window`/`build_frq_prelude`（仅 prelude 列扩充 + 多窗口循环）；merge byte-copy 等高级特性不在本期。

## 6. 读取改动

### 6.1 frq_prelude reader
新增访问器：`frq_window_offset(w)`/`frq_window_len_bytes(w)`（从 Foff）、`prx_window_offset(w)`/`len`（从 E）、`win_base(w)`（C 累积）、`doc_count(w)`（D）、`super_block(s)`（SB）。

### 6.2 phrase_query（核心改动：窗口跳读）
现状「读全段 → 全解码 → 交集」改为「读 prelude → 候选驱动选窗口 → 批量取命中窗口 → 交集」：
1. lookup 各 term → DictEntry + frq_base/prx_base。inline/slim term 照旧（小，直接解码）。
2. 对每个 windowed term，第 1 轮批量读其 **prelude**（小；若 has_sb 且 prelude 大，先读 SB+相关 prelude 切片）。解析窗口目录（C/D/Foff/E）。
3. 选 **lead term = df 最小者**；读其全部窗口（小）得候选 docid 序列。
4. 对其余 term：对候选 docid 在其 prelude `last_docid` 列二分，定位覆盖窗口集合（去重）。第 2 轮 `BatchRangeFetcher` 批量并发取这些窗口的 `.frq`(+`.prx`) 子 range。
5. 解码命中窗口，取候选 doc 的 freq/positions，做位置邻接交集（沿用现有 `PhraseInDoc`）。

效果：高频词只读「覆盖候选 doc 的窗口」而非整段 → 字节数从 O(df) 降到 O(候选数×窗口大小)。轮次 ≈ 2（preludes 批量 + 选中窗口批量），保持低轮次。

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

1. **格式**：frq_prelude v2（加 `frq_cum_off` 列 + 访问器 + SB 结构 + 测试）。
2. **写入**：logical_index_writer 多窗口 + prelude/SB 构建（read-back 自校验：用 prelude 定位每窗口并解码一致）。
3. **读取**：phrase_query 窗口跳读（差分测试 vs 全读/oracle，含多窗口高频词短语）。
4. **验证**：全量测试 + 5M `--oss` 重测短语，更新基准报告。

（super-block SB 目录可作为第 1/3 期内的子项；若 prelude 单读已足够小，可先实现 per-window 跳读，再补 SB 粗目录避免读全 prelude。）
