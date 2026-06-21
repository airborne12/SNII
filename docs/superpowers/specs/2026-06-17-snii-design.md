# SNII：Doris S3-Native 倒排索引 C++ 库设计规格

- 状态：已批准（待用户复核 spec）
- 日期：2026-06-17
- 来源规格：[`docs/design/SNII-design-spec.source.md`](../../design/SNII-design-spec.source.md)（飞书原文存档）
- 方法论：superpowers `brainstorming → writing-plans → test-driven-development`

---

## 1. 背景与目标

Apache Doris 现有倒排索引（V1/V2/V3）以本地磁盘为默认假设，基于 CLucene 子文件（`.tis/.tii/.frq/.prx`）组织 term 字典、doc/freq postings 与 positions。在 S3 等对象存储上，查询执行随 postings/positions iterator 推进逐步 seek，触发大量小范围 `read_at()`；即便 FileCache 按 1MB 块对齐合并，**关键路径上的串行 I/O 轮次**仍然偏多。

**SNII 目标**：把倒排索引设计成 S3-native 格式——读路径先依赖常驻元数据生成 S3 友好的预读/并发规划，再批量并发读取词典、倒排表、位置信息，从而把三个核心指标压下来：

1. **串行 I/O 轮次**（serial I/O rounds）：决定查询延迟下限与 p99。
2. **Range GET 数**：决定请求计费与远端 QPS。
3. **读取字节数**：决定带宽、缓存占用、解码 CPU。

设计优先级：**先降串行轮次与远端请求数，其次才是减少字节数**（存算分离下 file cache 与 S3 单位字节约 10:1，可牺牲索引体积换冷查性能）。

## 2. 范围与非目标

### 2.1 范围（本期实现 = 全量保真）

完整落地源规格的容器格式与读写路径：

- 容器格式：bootstrap header、streamed data sections、norms & null-bitmap PODs、tail meta region、fixed tail pointer。
- 词典：DICT block（字节切分 + 前缀压缩 + 锚点 + crc）、DictEntry（inline/pod_ref × slim/windowed）、SampledTermIndex、DICT block directory、binary-fuse-8 XF（不存在 term 过滤）。
- 倒排：`.frq` POD（自适应窗口 256/512/1024/2048、列式 dd+freq、PFOR、win_base 可 byte-copy stitch）、列式 prelude（窗口元信息 + 可选 super-block 目录）、`.prx` POD（positions，默认 PFOR——位打包 pos_count 列 + position-delta；zstd/raw 保留）。
- 统计/打分：norms POD（1B/doc encoded norm）、StatsBlock、SniiStatsProvider、基础 BM25 + 窗口级 block-max 剪枝（WAND 风格）。
- 读取规划：term lookup（XF → SampledTermIndex → DICT block → DictEntry）、BatchRangeFetcher（合并 + 并发）、term / phrase / scoring 查询。
- 写入：SPIMI buffer + spill + final k-way merge（term 有序、格式一致、窗口 byte-copy stitch）、单遍 append-only 写出。
- 倒排存储内容按 logical index 配置：`docs-only` / `docs-positions` / `docs-positions-scoring`。

### 2.2 非目标（本期不做 / 预留）

- `positions-offsets`（char offsets，面向高亮/RAG）：源规格标注「预留」，本期仅占位 feature bit，不实现。
- 与 Doris BE 真实集成（SegmentWriter/IndexFileReader 接管）：本期为独立 lib + 基准，不改 Doris 源码。
- 分布式/跨 segment 合并执行器：仅实现 SNII 单 segment 内的 k-way merge 与跨 segment 统计聚合公式。

## 3. 架构

### 3.1 分层（高内聚低耦合，每单元独立可测）

```
L0 编码原语   varint/zigzag · crc32c · PFOR/FOR · zstd 包装 · ByteSink/ByteSource · SectionFramer
L1 I/O 抽象   FileReader / FileWriter(append-only) · LocalFile · S3(aws-sdk) · MeteredFileReader · BatchRangeFetcher
L2 格式模型   bootstrap_header · tail_pointer · dict_entry · dict_block · sampled_term_index ·
              dict_block_directory · xfilter · frq_pod · frq_prelude · prx_pod · norms_pod ·
              null_bitmap · stats_block · per_index_meta · logical_index_directory · tail_meta_region
L3 写/读编排  spimi_term_buffer · logical_index_writer · snii_compound_writer ·
              snii_segment_reader · logical_index_reader · term_lookup ·
              posting_decoder · position_decoder · term_query · phrase_query · bm25_scorer · stats_provider
L4 基准(独立) corpus_gen · clucene_adapter(TracingDirectory) · snii_adapter · runner
```

### 3.2 目录结构与依赖规则

```
SNII/
├── core/                 # libsnii —— 零 bench / 零 clucene 依赖
│   ├── include/snii/     # 仅 public API
│   ├── src/              # 实现
│   └── tests/            # 仅 core 的 gtest 单测
├── bench/                # 独立 target：bench → libsnii(public) + clucene
├── third_party/          # vendored 单头：xxHash / binaryfusefilter / CRoaring
├── cmake/                # FindCLucene.cmake / FindAwsSdk.cmake
└── docs/
```

**依赖单向铁律**：`core` 绝不 `#include` 任何 bench/clucene 头；`core` 可单独编译并跑全部单测。`bench` 仅通过 public 头依赖 `libsnii`。`MeteredFileReader`（S3 成本模型）属于 core 的 L1（SNII 与 CLucene 两侧共用同一把尺）；CLucene 专属 `TracingDirectory` 只在 bench。

### 3.3 DRY 抽象基座（结构性杜绝面条/重复）

- **单一序列化游标**：`ByteSink`（append-only 写）/ `ByteSource`（slice 读）——所有 section 复用，禁止各自手拼 varint/crc。
- **统一 section 框定**：`SectionFramer`（header + payload + crc32c 的封装/校验）一处实现，全 section 复用。
- **DictEntry 三态**（inline/pod_ref × slim/windowed）→ 策略 + tag dispatch，不在一个函数堆 if-else。
- **codec 派发**（raw/zstd/PFOR）→ codec registry / dispatch 表，不写 switch 长链。
- **section 解析** → 注册式 visitor，新增 optional section 不动主流程。

## 4. 格式规格（要点；逐字节布局以源规格为准）

### 4.1 容器布局

`{rowset_id}_{seg_id}.idx`，作用域 = Doris segment，含多个 logical index，用 `(index_id, index_suffix)` 定位。

```
[bootstrap header]          magic/format_version/header_length/flags/tail_pointer_size/min_reader_version/header_checksum
[streamed data sections]    每 logical index: DICT blocks · .frq POD · .prx POD
[norms & null-bitmap PODs]  每 logical index: encoded_norm POD · null bitmap(Roaring) POD
[tail meta region]          per-index meta block* · logical index directory · meta_region_checksum
[fixed tail pointer]        magic/format_version/meta_region_offset/meta_region_length/hot_off/checksums/tail_pointer_size/tail_checksum
```

### 4.2 关键 section（实现要点）

- **DICT block**：按编码后字节数切分（`target_dict_block_bytes`，构建期参数，不固化为格式语义）；只在 term 边界切；inline posting 计入块大小、外部 payload 不计入；UTF-8 字节级前缀压缩；周期写完整 term 锚点 + 锚点偏移表；尾部 crc32c 覆盖 header+entries+anchors。
- **DictEntry**：`entry_len` 自描述长度；term key（prefix/suffix front-coding）；flags（kind=pod_ref/inline、enc=slim/windowed、has_sb、has_champion 恒0、offsets_ref 恒0）；term stats（df、ttf_delta、max_freq，tier≥T2）；payload locator（pod_ref: frq/prx off_delta+len、prelude_len；inline: frq/prx bytes）。
- **XF（不存在 term 过滤）**：binary-fuse-8，位于 per-index meta；`xf_bytes≤256KB` 入 L0 常驻，否则 L1；`term_count>32M` 可省略（L0 标 `xf_absent`）；仅用于 exact term。
- **SampledTermIndex**：采样粒度 = DICT block（每 block 的 first_term），规模与 block 数成正比；与 DICT block directory 按 ordinal 对齐。
- **`.frq` 窗口**：256-doc unit 组合成 256/512/1024/2048；列式（先全 dd-PFOR run，再全 freq-PFOR run）；`win_base(w)=win_last(w-1)`，非首窗存相对 delta，支持 merge byte-copy；窗口头 `win_mode/uncomp_len/comp_len/dd_part_len/crc32c`，`dd_part_len` 让 bitmap 路径跳过 freq。
- **prelude**（windowed `.frq` 前置目录，字节属 `.frq` 域）：列式 `max_freq/max_norm/last_docid_delta/frq_window_len/prx_cum_off/win_crc32c` + 可选 super-block 目录；提供查询计划（C/D/E 列定位窗口）、剪枝（B/B2 算窗口上界）、merge（按列重基/修正/重算 crc）。
- **`.prx` 窗口**：独立分窗，与 `.frq` 在 256-doc unit 边界可对齐；header `codec/uncomp_len/comp_len/crc32c`；**默认 PFOR**（位打包 pos_count 列 + position-delta，体积反超 zstd 且构建 CPU 更低；zstd/raw 保留为回放/强制路径）；读侧 `read_prx_window_csr` 解进扁平 CSR 列供游标流式短语求值；超大单 doc positions 退化为单大窗口。
- **slim/inline 形态**：`df<512`→slim（紧凑 VInt，无 prelude）；`slim && bytes≤inline_threshold`→inline（全写入 DictEntry）；否则 pod_ref slim。
- **norms POD**：per logical index/field，1B/doc encoded doc length。
- **per-index meta block**：PerIndexMetaHeader + StatsBlock + SampledTermIndex + DICT block directory + optional XF + SectionRefs（dict/frq/prx/norms/null_bitmap ref）+ feature bits + checksums；随 `SniiLogicalIndexReader` 进 searcher cache，按实际字节计费。

### 4.3 兼容与校验

- 自描述、可校验、可演进：unknown **required** feature → **拒读**；unknown **optional** section → **可跳过**。
- 每块 crc32c，reader 按需校验；fixed tail pointer 含多重 checksum（meta_region/bootstrap/tail）。

## 5. I/O 抽象与三指标方法论

### 5.1 接口

```cpp
class FileReader {            // 唯一物理读原语
 public:
  virtual Status read_at(uint64_t offset, size_t len, Slice* out) = 0;
  virtual uint64_t size() const = 0;
};
class FileWriter {            // append-only，禁止 seek 回写
 public:
  virtual Status append(Slice data) = 0;
  virtual Status finalize() = 0;
  virtual uint64_t bytes_written() const = 0;
};
```

后端：`LocalFileReader/Writer`、`S3FileReader/Writer`（aws-sdk-cpp，Range GET / 缓冲后 PutObject）。

### 5.2 S3 成本模型（`MeteredFileReader`，core 内可复用装饰器）

包装任意 `FileReader`，对每次物理读建模：

- 维护 1MB block 对齐的 FileCache 驻留集；`read_at` 命中的 block 中，miss 的相邻 block 合并为一次 Range GET。
- 统计：① `read_at()` 调用数 ② **串行轮次** ③ Range GET 数 ④ 远端字节数（去重后）+ 原始字节数。
- 两种提交语义：
  - **单读阻塞**（CLucene 路径）：每次 `read_at` 若含 ≥1 miss → +1 串行轮次（cursor 串行，下一个 offset 必须等本次返回）。
  - **批量提交**（SNII 路径）：`submit_batch([ranges])` 合并所有 miss → +1 串行轮次（批内并发）。

### 5.3 BatchRangeFetcher（SNII 读路径）

收集多 term/多段产生的 `.frq/.prx/norms` ranges → 合并相邻/重叠 → 线程池并发提交 `FileReader::read_at` → 回填缓冲。每次 `submit` 在 `MeteredFileReader` 记 1 串行轮次。

## 6. 写入设计

```
SegmentWriter → SniiCompoundIndexWriter
  写 bootstrap header（仅容器级固定字段）
  每列索引/子列 → LogicalIndexWriter(key=(index_id,index_suffix), 输入=token stream/null bitmap/doc length)
    SPIMI 累积：整数 term-id 键入 CompactPostingPool —— 共享 bump-arena，每 posting 编码为
      tagged-varint 流（每 token = tag 位 + 位置 delta；每新 doc = zigzag docid delta），
      比 raw uint32 向量小 ~3.4×，无 per-token 字符串哈希
    内存超 spill 阈值 → 落一个磁盘 run（raw u32 docids/freqs/positions，4MiB 写缓冲）；
      reset arena 归还 OS + malloc_trim → 继续累积下一批
    final k-way merge：按 vocab 串有序合并各 run + 末批内存项；逐 term 跨 run coalesce docid/freq；
      宽 term 的 positions 经 pos_pump 跨 run 游标流式喂给 writer，不物化数十 MB 级位置瞬态
    流式输出：DICT blocks / .frq POD / .prx POD 各写独立临时段，finish 时拼接，内存 manifest 记 refs/codec/checksum/stats
  写 side PODs：encoded_norm POD（per field）、null bitmap POD（per logical index）
  构建并写 per-index meta blocks（StatsBlock/term locator/HighDfDirectory/XF/section refs/feature bits/checksums）
  写 logical index directory（(index_id,index_suffix)→per-index meta ref）
  写 meta_region_checksum → 追加 fixed tail pointer → close
```

不走 V2 两阶段打包：单遍流式写入，close 前只追加 meta 与 tail pointer。索引输出 **byte-identical** 于 spill 阈值（含无 spill 全内存路径）—— spill 只 bound 内存、不改产物，由专门测试跨 {无 spill / spill+宽词常驻 / 跨缝边界 doc} 断言。

**构建资源模型**（S3-native 设计的写端约束，与读端三指标并列）：内存峰值可调（spill 阈值），两段都被压低 ——① 累积期由 CompactPostingPool 紧凑表示压低，② merge 期由宽 term 位置流式消除单个高频 term 的数十 MB 物化瞬态（否则该瞬态与 spill 阈值无关地占据 merge 峰值）。因此 bounded build 的引擎内存随规模次线性增长：真实日志 50M 实测引擎内存约为 CLucene 的 1/7（详见 `benchmark-results.md`）。磁盘体积比 CLucene 小 ~13%（`.prx` PFOR + DICT 块 zstd），构建 CPU 在 ≥20M 规模反超 CLucene。这与「牺牲索引大小换冷查」的早期假设相比已无需牺牲——SNII 在体积、内存、CPU 三项均不弱于 CLucene，同时保留读端的批量 range 优势。

## 7. 读取与查询设计

```
SegmentReader 打开 .idx（file_size 优先用 rowset meta 的 index_size）
  定位 tail meta：有 hot_off → 读 [hot_off,EOF) 1 轮；否则读 tail pointer + tail meta region 2 轮
  校验 tail magic / meta_region_checksum / bootstrap checksum / section 边界 / feature bits
  SniiSegmentReader：解析 logical index directory → map<(index_id,suffix), PerIndexMetaRef>
  open(index_meta) → SniiLogicalIndexReader（持 per-index meta，进 searcher cache，LRU 淘汰）
查询执行：
  term lookup：XF → SampledTermIndex 二分定位 block ordinal → DICT block directory 取 offset/len →
               一次 range read DICT block + crc 校验 → block 内锚点+局部扫描定位 DictEntry
  生成读取计划：inline 直接解码 / pod_ref 得 .frq(+.prx) ranges / scoring 关联 norms+stats
  BatchRangeFetcher 合并并发读 payload → posting/position decoder → bitmap 或 score
```

- **term query**：→ Roaring docid bitmap。
- **phrase query（计划取窗 + 游标流式求值）**：两段式。①**计划阶段 docid 先行**——driver（最小 df）取全窗 dd、其余 term 取覆盖候选窗口的 dd（`.prx` 同批一次取回备用），docid-only 求交得候选，**整段跳过 freq-block**，位置不在此解码；串行轮次 ≈ 每 term 一批、与命中量无关。②**求值阶段游标流式单遍**——每 term 一个 `PostingCursor` 在已取回的内存窗口上单调前进，位置按候选**局部下标懒解**（`.prx` 每窗解一次进 `pos_flat`/`pos_off` 扁平 CSR 列，PFOR 顺序解码、无逐 doc 堆分配），相邻性逐 start 校验、词级短路；**无逐候选 docid 二分、无全候选位置物化**，位置只为幸存候选所在窗解码。详见源规格「短语查询执行」。
- **scoring（BM25）**：SniiStatsProvider 提供 df/ttf/avgdl/encoded_norm；窗口级 `max_freq/max_norm` 算 block-max 上界，WAND 风格决定跳过/读取窗口。上界必须不低估真实最高分。

## 8. 基准设计（与 core 物理隔离）

- **corpus_gen**：Zipfian 词分布合成语料（固定 seed 可复现），5M docs；产出 term 流供两侧索引器消费；可控 df 分布与查询选择度。
- **clucene_adapter**：用 CLucene API 构建真实索引（`.tis/.tii/.frq/.prx`）；`TracingDirectory` 包装 `FSDirectory`，把每次 `BufferedIndexInput::readInternal` 物理读喂入共享 `MeteredFileReader`。
- **snii_adapter**：用 libsnii 构建 SNII 索引并检索。
- **runner**：构建两份索引 → （可选）上传真实 OSS → 跑查询套件（exact term、`MATCH`、5-term `MATCH_PHRASE`、scoring）→ 采集四指标 → 输出对比表（markdown + CSV）。
- **双轨 S3**：① 确定性成本模型（可复现四指标）；② 真实 OSS（aws-sdk Range GET 回读，wall-clock 延迟，验证「串行轮次×RTT」）。endpoint 用外网 `oss-cn-hongkong.aliyuncs.com`；凭证仅从环境变量读取（见 §11）。
- **正确性闸门**：信任性能数字前，先断言 SNII 与 CLucene 对同一查询返回**相同 docid 集合**。

## 9. 代码质量验收门（强制）

| 项 | 标准 |
|---|---|
| 函数长度 | 默认 ≤50 行，硬上限 ≤80（远低于 200 面条线）|
| 分支 | if-else 链 → 多态/策略/dispatch；early-return；嵌套 ≤3 层 |
| 重复 | 零复制粘贴；公共逻辑下沉 L0/L1（DRY）|
| 文件 | 单一职责，200–400 行典型，≤800 |
| 错误 | 全程 `Status`，无静默失败；RAII，无裸 `new`，无 `using namespace` 于头文件 |
| 命名 | 一致、自解释；遵循 core 既定风格 |
| 测试 | 每模块先 RED 单测；≥80% 覆盖；oracle 交叉校验 |
| 把关 | 每阶段实现后过 code-reviewer / security-reviewer agent |

## 10. 测试策略（TDD）

- **L0/L1**：round-trip 属性测试（varint/pfor/crc/zstd/ByteSink-Source）、边界、损坏检测（翻字节 → crc fail）。
- **L2**：每 section write→parse round-trip；前向兼容（unknown optional section 跳过）；checksum 校验。
- **L3**：朴素内存倒排索引作 ground-truth oracle，断言 term/phrase/score 结果一致；writer→reader 端到端小索引。
- **成本模型**：对已知访问模式断言串行轮次/Range GET/字节数。
- **基准**：SNII vs CLucene docid 集合一致性作为正确性闸门。
- RED→GREEN→REFACTOR 严格执行，每模块先写失败测试。

## 11. 构建与安全

- CMake；`core` 与 `bench` 各自 `CMakeLists.txt`，顶层 `option(SNII_BUILD_BENCH)`、`option(SNII_WITH_S3)`。
- `FindCLucene.cmake`：定位预编译 `libclucene-{core,shared,contribs}-static.a`（`doris-clean/be/build_Release/bin`）+ doris-thirdparty 头 + 生成的 `clucene-config.h`。
- 链接系统 zstd/lz4；真实 S3 经 `SNII_WITH_S3` 链接 aws-sdk-cpp（doris thirdparty installed）。
- vendored 单头：xxHash、`binaryfusefilter.h`、CRoaring amalgamation。
- **安全**：OSS AK/SK 仅从环境变量（`SNII_OSS_AK/SK/ENDPOINT/BUCKET/PREFIX`）读取；`.gitignore` 屏蔽任何本地凭证文件；**绝不入库、绝不硬编码、绝不打印明文**。

## 12. 里程碑（提交即闭环：业务码 + 单测同批）

1. **M0 脚手架**：CMake、core/bench 隔离、gtest 接入、CI 编译跑通空测试。
2. **M1 L0 编码原语**：ByteSink/Source、varint、crc32c、PFOR、zstd、SectionFramer（全 round-trip 测试）。
3. **M2 L1 I/O**：FileReader/Writer、LocalFile、MeteredFileReader、BatchRangeFetcher。
4. **M3 L2 词典族**：dict_entry、dict_block、sampled_term_index、dict_block_directory、xfilter。
5. **M4 L2 倒排族**：frq_pod、frq_prelude、prx_pod、norms_pod、null_bitmap、stats_block。
6. **M5 L2 容器/meta**：bootstrap_header、per_index_meta、logical_index_directory、tail_meta_region、tail_pointer。
7. **M6 L3 写**：spimi_term_buffer、logical_index_writer、snii_compound_writer（端到端小索引）。
8. **M7 L3 读/查询**：segment_reader、logical_index_reader、term_lookup、decoders、term/phrase/bm25。
9. **M8 S3 后端**：S3FileReader/Writer（aws-sdk），真实 OSS 读写打通。
10. **M9 基准**：corpus_gen、clucene/snii adapter、runner、双轨四指标对比报告。

## 13. 风险与缓解

- **CLucene 独立驱动复杂度**：标准 CLucene 多文件索引足以复现 `.tis/.tii/.frq/.prx` cursor-seek 模式；如 Doris V2 compound 驱动过重，则用标准 CLucene 索引 + 可选 compound 打包。
- **PFOR/格式细节庞大**：按里程碑分批，每批闭环；编码先正确后优化，字节数指标以正确编码为准。
- **真实 OSS 抖动**：成本模型给确定性指标，OSS 给趋势验证；多次取中位数。
- **5M 构建耗时**：corpus/index 产物缓存复用；规模可参数化下调以快速迭代。
