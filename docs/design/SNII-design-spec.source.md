# SNII：Doris S3-Native 倒排索引结构设计

# 背景与目标
Apache Doris 当前倒排索引的存储与读取路径仍以本地磁盘为默认假设。V1/V2/V3 依赖 CLucene 子文件，最终被打包进现有 compound `.idx` 文件，读取时仍通过 `.tis/.tii/.frq/.prx` 等 Lucene 风格子文件组织 term 字典、doc/freq postings 和 positions。<text bgcolor="light-yellow">这套结构在本地磁盘上可接受，但在 S3 等对象存储上，查询执行会随着 term postings / positions iterator的推进逐步seek下一段 payload offset，从而触发大量小范围 </text><text bgcolor="light-yellow">`read_at()`</text><text bgcolor="light-yellow">。</text>
<text bgcolor="light-yellow">以 15M 行 segment 上 5 个 term 的 </text><text bgcolor="light-yellow">`MATCH_PHRASE`</text><text bgcolor="light-yellow"> 为例，</text><text bgcolor="light-yellow">当前读取路径会随着 postings / positions iterator推进触发数千次 </text><text bgcolor="light-yellow">`FileReader::read_at()`</text><text bgcolor="light-yellow">。虽然 FileCache 会按 1MB block 对齐并合并这些小读，使实际远端 Range GET 降到几十次，但这些 GET 的 offset 多数需要在查询执行过程中逐步解析，难以前置规划和并发提交，因此关键路径上的</text><text bgcolor="light-yellow">**串行 I/O 轮次**</text><text bgcolor="light-yellow">仍然偏多</text><text bgcolor="light-yellow">。</text>
SNII 的目标是将倒排索引设计成 S3-native 格式：读路径先依靠元数据生成<text bgcolor="light-yellow">S3友好的预读，并发规划</text>，再批量并发读取词典，倒排表，位置信息等。把关键路径串行 I/O 轮次、远端请求数量和 BE 内部 `read_at()` 调用数降低下来，代价可能是增加倒排索引数据大小，换取冷查性能，在存算分离模式下，file cache和S3的单位字节价格大概是10:1，牺牲一些索引大小换取更少的file cache。
例如 5 个 term 的 `MATCH_PHRASE`：当前路径里，执行器可能先读 term dictionary，再读某个 term 的 postings，再读取候选 doc，再为了 phrase 判断去读 positions。很多 `.prx` 的读取是在 phrase scorer 运行到某些 doc 后才触发的。
<text bgcolor="light-yellow">假设一个查询最终需要读 32MB，如果这 32MB 可以在 1 轮里并发发出 32 个 1MB Range GET，延迟大致接近最慢那个 GET。而如果这 32MB 被拆成先读一点，解码后才知道下一批 offset，再读下一点，再解码，再读 positions，那么总延迟会变成串行IO叠加。</text>
## 评估指标
对象存储读路径必须同时评估三个指标：

<lark-table rows="5" cols="3" header-row="true" column-widths="231,244,244">

  <lark-tr>
    <lark-td>
      指标 {align="center"}
    </lark-td>
    <lark-td>
      含义 {align="center"}
    </lark-td>
    <lark-td>
      影响 {align="center"}
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      串行 I/O 轮次 {align="center"}
    </lark-td>
    <lark-td>
      后续读取依赖前一批数据返回而形成的顺序等待轮数 {align="center"}
    </lark-td>
    <lark-td>
      查询延迟下限、p99 {align="center"}
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      Range GET 数 {align="center"}
    </lark-td>
    <lark-td>
      查询发起的远端 range 请求数量 {align="center"}
    </lark-td>
    <lark-td>
      请求计费、远端 QPS {align="center"}
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      读取字节数 {align="center"}
    </lark-td>
    <lark-td>
      所有 range 返回的总字节量 {align="center"}
    </lark-td>
    <lark-td>
      带宽、缓存占用、解码 CPU {align="center"}
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      并发查询 {align="center"}
    </lark-td>
    <lark-td>
      并发查询下的资源消耗 {align="center"}
    </lark-td>
    <lark-td>
      {align="center"}
    </lark-td>
  </lark-tr>
</lark-table>

同一轮内的多个 range 可以并发；只有下一批 range 必须等待上一批返回才能生成时，才增加串行轮次。<text bgcolor="light-yellow">SNII 的收益模型和测试必须按这三个指标分布统计</text>。
## 目标与设计约束
SNII 的目标不是把现有倒排索引文件简单搬到对象存储上，而是重新组织词表、倒排表和footer元数据，使读取路径在访问大块倒排数据前，能够先确定本次查询需要读取的范围，并批量并发提交S3读取。
具体目标如下：
- 对精确term查询，应尽量通过常驻元数据完成term不存在判断；若term可能存在，再定位到对应的词表或倒排表位置。不存在的term应能在 segment 级快速跳过。
- 低频term的倒排表通常很短，应尽量随词表一起保存。读取词表后即可获得该term的命中doc id、词频和位置信息，避免为少量倒排数据再发起额外读取。
- 高频term的倒排表和位置信息应按接近S3缓存块的粒度组织（比如1MB），使查询可以一次性规划相关读取范围。
- 短语查询应在计划阶段同时安排命中doc id、词频和位置信息的读取，避免进入位置判断阶段后再触发串行小规模数据读。
- 相关性打分查询必须由 SNII 原生统计信息、长度归一化信息和倒排表语义支撑，不能只把 `MATCH_*` 查询降成 Roaring bitmap。
- 低频term和高基数列这种场景下，设计应控制大词表下的常驻元数据体积，并让短倒排表尽量随词表完成读取，避免小倒排数据被对象存储读取粒度放大。
同时，格式和实现必须满足以下约束：
- SNII 是新的原生倒排索引格式，重写代码，不再依赖clucene源代码。
- Writer 必须支持 append-only 输出，不能依赖 seek 回写，以适配 `S3FileWriter`、`StreamSinkFileWriter` 和 packed writer。
- 格式必须自描述、可校验、可演进；未知 required feature 必须拒读，未知 optional section 必须可跳过。
- Spill merge 和 final k-way merge 必须保持term有序、倒排表语义一致和格式兼容。
- 设计优先级是降低关键路径串行 I/O 轮次和远端请求数，其次才是减少读取字节数；
# 详细设计
## 总览
SNII container 的作用域是 Doris segment。单个 `{rowset_id}_{seg_id}.idx` 包含多个 logical index，每个 logical index 继续用 `(index_id, index_suffix)` 定位，保持与现有 `IndexFileReader`、`InvertedIndexDescriptor`、tablet schema 里的 key 体系兼容。
`.frq`、`.prx` 在本文中是逻辑 POD 名称，分别表示 doc/freq payload 和 positions payload，不沿用 CLucene 子文件格式，也不保留 `_0.tis` 这类 Lucene 内部命名。
<image token="KkRpbbOyIoEqF9xNRhTc8aqhnmT" width="1672" height="941" align="center"/>

```plaintext
SNII container: one Doris segment ({rowset_id}_{seg_id}.idx)

  [bootstrap header]
    magic / format_version / header_length / flags
    tail_pointer_size / min_reader_version / header_checksum

  [streamed data sections]
    logical index #1: DICT blocks, .frq POD, .prx POD
    logical index #2: DICT blocks, .frq POD, .prx POD
    ...

  [norms & null-bitmap PODs]
    logical index #1: encoded_norm POD, null bitmap POD (Roaring)
    logical index #2: encoded_norm POD, null bitmap POD (Roaring)
    ...

  [tail meta region]
    per-index meta block #1:
      StatsBlock
      SampledTermIndex / DICT block directory
      optional XF
      dict_section_ref / frq_ref / prx_ref / norms_ref / null_bitmap_ref
      feature bits / checksums

    per-index meta block #2:
      StatsBlock
      SampledTermIndex / DICT block directory
      optional XF
      dict_section_ref / frq_ref / prx_ref / norms_ref / null_bitmap_ref
      feature bits / checksums

    ...

    logical index directory:
      (index_id, index_suffix) -> per-index meta ref

    meta_region_checksum

  [fixed tail pointer]
    magic / format_version / meta_region_offset / meta_region_length
    hot_off / meta_region_checksum / bootstrap_header_checksum
    tail_pointer_size / tail_checksum
```

- `bootstrap header`：文件头固定字段，用于识别 SNII container 和基础兼容性检查。
- `streamed data sections`：按 logical index 顺序写入词表、命中doc id/词频和位置信息。
- `norms & null-bitmap PODs`：集中存放但按 logical index 独立引用的长度归一化信息和 NULL bitmap。
- `per-index meta block`：保存单个 logical index 的查询规划元数据，并按实际字节数进入 searcher cache。
- `logical index directory`：container 级目录，把 `(index_id, index_suffix)` 映射到对应的 per-index meta。
- `fixed tail pointer`：文件尾固定入口，用于定位 tail meta region 和校验热区。
- norms 和 null bitmap 单独设计，是为了把 per-doc 的辅助数据从 per-term 的词表和倒排数据中解耦，使 DICT block 保持轻量，tail meta 保持常驻可控，同时让 scoring 和 NULL 处理按需把对应 side POD 加入批量读取计划。
### 写入流程
<image token="OUxIbb1rNo57l3xUCoucIHm4nNh" width="1672" height="941" align="center"/>

```plaintext
1. SegmentWriter 创建 SniiCompoundIndexWriter
   输出仍是 segment 级 {rowset_id}_{seg_id}.idx。

2. 写 bootstrap header
   只包含 container 级固定字段，不依赖后续 section 大小。

3. 为每个列索引/子列索引创建 logical index writer
   key = (index_id, index_suffix)
   输入 = analyzer token stream / null bitmap / doc length。

4. 每个 logical index writer 执行 SPIMI
   累积：整数 term-id 键入 CompactPostingPool（共享 bump-arena 的 tagged-varint 流，
     每 token = tag 位 + 位置 delta，每新 doc = zigzag docid delta，比 raw uint32 小 ~3.4×）。
   超 spill 阈值则落一个 raw-u32 磁盘 run（docids/freqs/positions，4MiB 写缓冲），
     reset arena 归还 OS + malloc_trim 后继续累积下一批。
   final k-way merge：按 vocab 串有序合并各 run + 末批内存项，逐 term 跨 run coalesce；
     宽 term 的 positions 经 pos_pump 跨 run 游标流式喂给 writer，避免数十 MB 物化瞬态。
   spill run 是私有临时格式（非 native，仅供回读合并）；只有最终 section 是 native。
   输出 byte-identical 于 spill 阈值（含无 spill 全内存路径）—— spill 只 bound 内存、不改产物。

5. final k-way merge 后顺序写 data sections
   写 DICT blocks。
   写 .frq POD。
   写 .prx POD。
   同步在该 logical index 的内存 manifest 中记录 dict_ref、frq_ref、prx_ref、codec、checksum 和统计信息。

6. 写 side PODs
   按 logical index / field 写 encoded_norm POD。
   按 logical index 写 null bitmap POD。
   在对应 manifest 中记录 norms_ref 和 null_bitmap_ref。

7. 构建并写出 per-index meta blocks
   每个 logical index 一个 meta block。
   内容包括 StatsBlock、term locator、HighDfDirectory、optional XF、section refs、feature bits 和 checksums。
   该 meta block 后续随 SniiLogicalIndexReader 进入 searcher cache，并按实际字节数计费。

8. 写 logical index directory
   container 级目录。
   记录 (index_id, index_suffix) -> per-index meta block ref。
   目录不承载具体词表或倒排数据，只负责定位 logical index 的 meta。

9. 写 meta_region_checksum，追加 fixed tail pointer，然后 close
   fixed tail pointer 记录 meta_region_offset、meta_region_length、hot_off、checksum 等。
   rowset meta 记录 index_size、meta_region_offset、meta_region_length、hot_off。
```

<text bgcolor="light-yellow">该流程保留 Doris “多个 logical index 合并成一个 segment 级 </text><text bgcolor="light-yellow">`.idx`</text><text bgcolor="light-yellow"> 文件”的语义，但不再走现有 V2 的两阶段打包：不先落 CLucene 子文件目录，也不再复制子文件到 compound。SNII writer 是单遍流式写入，close 前只追加 meta 和 tail pointer。</text>
### 读取流程
<image token="QNtObdLzKozS74xvQKWc0FSgnzc" width="1672" height="941" align="center"/>

```plaintext
1. SegmentReader 根据 segment path 打开 {rowset_id}_{seg_id}.idx
   file_size 优先使用 rowset meta 的 InvertedIndexFileInfo.index_size。

2. 定位 tail meta region
   rowset meta 有 hot_off:
     直接读 [hot_off, EOF)，1 轮。
   否则:
     先读 fixed tail pointer 得到 meta_region_offset / meta_region_length / hot_off，
     再读 tail meta region，2 轮。

3. 校验 container 与 meta region
   校验 tail magic、meta_region_checksum、bootstrap header checksum、section 边界和 feature bits。

4. 构建 SniiSegmentReader
   解析 logical index directory。
   建立 map<(index_id, index_suffix), PerIndexMetaRef>。
   SniiSegmentReader 共享不可变 FileReader 和 container 级元数据。

5. open(index_meta) 构建 SniiLogicalIndexReader
   根据 (index_id, index_suffix) 定位并读取对应 per-index meta block。
   per-index meta block 包含 StatsBlock、term locator、HighDfDirectory、optional XF、section refs 和 checksums。
   SniiLogicalIndexReader 持有该 logical index 的查询规划元数据。

6. 进入 searcher cache
   SniiLogicalIndexReader 作为 searcher cache value 缓存。
   按 per-index meta 实际字节数计费。
   内存压力由现有 searcher cache LRU 统一淘汰。

7. 查询执行
   使用 optional XF / HighDfDirectory / SampledTermIndex / DICT block 定位 term。
   生成 .frq / .prx / norms / null bitmap 读取 ranges。
   BatchRangeFetcher 合并 range 并发读取 payload。
   原生 evaluator 解码倒排数据，返回 bitmap 或 score。
```

### 倒排索引存储内容配置
不同查询场景需要的倒排数据不同：只做过滤查询时可以只保存 docid；需要短语匹配时必须保存term位置信息；需要相关性打分时还必须保存词频、norms 和统计信息。
这种设计让同一套 SNII 文件格式可以覆盖不同成本和能力需求：轻量场景减少存储和读取开销，复杂文本检索场景则写入更多语义数据以支持 phrase 和 scoring。
SNII 的倒排索引存储内容按 logical index 配置，而不是按单个 term 动态变化。Doris 中 freq/positions 是否存在由 index property 单源决定：`support_phrase` 关闭时 writer 设置 `omit_term_freq_and_positions=true`，打开时写入 freq 和 positions。当前代码已经把该标志从 `InvertedIndexColumnWriter` 传入 `SpimiIndexWriter`、spill manager 和 final `SpimiFinishConfig`，SNII 继续保持这个不变量。

<lark-table rows="5" cols="4" header-row="true" column-widths="239,289,203,240">

  <lark-tr>
    <lark-td>
      配置 {align="center"}
    </lark-td>
    <lark-td>
      保存内容 {align="center"}
    </lark-td>
    <lark-td>
      支持场景 {align="center"}
    </lark-td>
    <lark-td>
      说明 {align="center"}
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      `docs-only` {align="center"}
    </lark-td>
    <lark-td>
      docid {align="center"}
    </lark-td>
    <lark-td>
      term / match 过滤 {align="center"}
    </lark-td>
    <lark-td>
      存储最小，不支持 phrase / scoring {align="center"}
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      `docs-positions` {align="center"}
    </lark-td>
    <lark-td>
      docid + freq + positions {align="center"}
    </lark-td>
    <lark-td>
      `MATCH_PHRASE` {align="center"}
    </lark-td>
    <lark-td>
      用位置信息判断词项顺序和距离 {align="center"}
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      `docs-positions-scoring` {align="center"}
    </lark-td>
    <lark-td>
      docid + freq + positions + norms + stats {align="center"}
    </lark-td>
    <lark-td>
      phrase + BM25 / scoring {align="center"}
    </lark-td>
    <lark-td>
      支持相关性排序和分数过滤 {align="center"}
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      `positions-offsets` {align="center"}
    </lark-td>
    <lark-td>
      positions + char offsets {align="center"}
    </lark-td>
    <lark-td>
      预留 {align="center"}
    </lark-td>
    <lark-td>
      面向高亮、RAG 引用、片段定位 {align="center"}
    </lark-td>
  </lark-tr>
</lark-table>

SNII 不把所有倒排索引都做成同一种重格式，而是允许 logical index 按查询需求选择保存 docid、词频、位置信息、norms 和统计信息，从而在存储成本、读取成本和查询能力之间做明确取舍。
## 词典设计
<image token="VVu9bAcmmoDdLlxZeXZczKVvnif" width="1672" height="941" align="center"/>

### 词典块
SNII 的词典块不仅用于判断 term 是否存在，也是一条查询生成倒排数据读取计划的入口。查询命中 term 后，词典项需要给出 docFreq、倒排表偏移、位置信息偏移以及必要的统计信息。对于低频 term，倒排数据可以直接内联在词典项中；对于高频 term，词典项通常只保存引用，指向较大的 .frq 数据区，如果该索引支持 phrase 查询，还会同时指向较大的 .prx 数据区。
<text bgcolor="light-yellow">在对象存储上，真正影响查询延迟的不是词表查找本身，而是命中 term 后要继续读取多少倒排数据、这些数据分布成多少个 range、能否提前合并以及能否并发读取。因此，SNII 将 DICT block 设计为 term 到倒排数据读取计划的定位单元，同时作为远端按需读取、缓存和 checksum 校验的基本单位。</text>
DICT block 按编码后的词表字节数切分，而不是像当前 CLucene `.tii/.tis` 一样按固定 term 数抽样。writer 按 term 字典序顺序生成 DictEntry，并维护当前 block 的序列化大小估算值。追加下一个 DictEntry 前，writer 会计算追加后的 block 大小：
```cpp
block_bytes =
  header + encoded entries + anchor offset table + footer / checksum
```

如果当前 block 非空，且追加后超过 `target_dict_block_bytes`，则先close当前 block，再从当前 term 开始创建新 block。`target_dict_block_bytes` 是构建期参数，具体默认值不作为格式语义固化，后续通过真实 segment 和对象存储读放大指标校准。
切分与编码规则：
- 每个 logical index 独立生成自己的 DICT block 序列。
- block 只在 term 边界切分，不拆分单个 DictEntry。
- inline posting 属于 DictEntry 的一部分，参与 block 大小计算。
- 外部 .frq / .prx payload 不参与 DICT block 大小计算，DictEntry 只记录 offset / length 引用。
- 单个 inline DictEntry 超过目标大小时，writer 优先改写为外部 posting 引用；仍然过大的 entry 可以独占一个 DICT block。
- term 文本使用 UTF-8 字节级前缀压缩。
- block 内周期性写入完整词项作为“词项锚点”，并维护锚点偏移表。reader 先在锚点上定位，再在局部 entry 范围内扫描，从而支持 exact term 查找、range 查询和 prefix 枚举。
- block 尾部写 crc32c，checksum 覆盖 header、entries 和锚点偏移表。reader 只有在按需读取该 block 时才校验它。
- **DICT block 默认 zstd 压缩**：dict region（term keys + DictEntry 元信息 + 97% 低频 term 的 inline 倒排）是索引最大 section，整块 zstd 同时**降索引体积**与**每次 term lookup 的 S3 读字节**（命中 term 只需取更小的压缩块）。`dict_block_directory` 每条记录一个 zstd 位 + `uncomp_len`；reader 取压缩块 → RAM 内解压 → 校验 crc → 锚点定位+局部扫描。压缩对 reader 透明，不改 DictEntry 语义。
DICT block 格式：
```cpp
DICT BLOCK
  header:
    n_entries          varint
    entry_format_ver   u8
    block_flags        u8
    frq_base           varint64
    prx_base           varint64   # 支持 positions 时存在

  entries[n_entries]
    variable-length DictEntry
    term text uses UTF-8 byte front-coding
    posting refs use offset / length relative to frq_base / prx_base

  anchor_offsets[]     u32 * n_anchors
  n_anchors            u32
  crc32c               u32
```

### 词典项
DictEntry 必须自描述长度，支持尾部追加字段：
```plaintext
DictEntry
  entry_len      varint

  term key:
    prefix_len   varint
    suffix_len   varint
    suffix_bytes u8[]

  flags          u8
    bit0 kind: 0=pod_ref / 1=inline
    bit1 enc:  0=slim / 1=windowed
    bit2 has_sb
    bit3 has_champion    # v1 恒 0
    bit4 offsets_ref     # v1 恒 0
    bit5-7 reserved

  term stats:
    df            varint
    ttf_delta     varint   # total_term_freq - df，仅 tier>=T2
    max_freq      varint   # term 级最大 tf，仅 tier>=T2

  payload locator:
    if kind = pod_ref:
      frq_off_delta  varint
      frq_len        varint
      prelude_len    varint  # enc=windowed
      prx_off_delta  varint  # tier>=T2 / positions enabled
      prx_len        varint  # tier>=T2 / positions enabled

    if kind = inline:
      frq_len        varint
      frq_bytes      u8[]
      prx_len        varint  # tier>=T2 / positions enabled
      prx_bytes      u8[]    # tier>=T2 / positions enabled
```

几个字段的核心作用：
- `entry_len`：让 reader 可以跳过未知扩展字段或快速跳过 entry。
- `prefix_len / suffix_len / suffix_bytes`：配合块内前缀压缩恢复 term。
- `flags.kind`：决定倒排数据是外部 POD 引用，还是直接内联在 entry 中。
- `flags.enc`：标识外部 posting 的编码形态，`windowed` 场景下可先读 prelude。
- `flags.has_sb`：表示 posting prelude 中带子块目录，可进一步拆分大 postings 的读取范围。
- `df / ttf_delta / max_freq`：用于过滤、scoring 和查询代价估算。
- `frq_off_delta / frq_len`：命中后生成 `.frq` range。
- `prx_off_delta / prx_len`：phrase/positions 查询时生成 `.prx` range。
- `inline frq_bytes / prx_bytes`：低频 term 直接在词典块内完成读取，不再访问外部 payload。
### 不存在的term快速过滤
位于 per-index meta block，使用 binary-fuse-8 作为 exact term 的不存在查找过滤。该结构适合 immutable segment：finish 时 term 集合已知，可以一次性构建。布放策略：
- `xf_bytes <= 256KB`：放入 L0 常驻。
- 超过 256KB：放入 L1，首查整体读取，之后依赖 FileCache。
- `term_count > 32M`：允许省略，L0 标记 `xf_absent`。XF 只用于 exact term。range、regexp、prefix、phrase_prefix 必须通过有序 term enum 展开，不能用XF裁剪。
### 词典采样索引
SampledTermIndex 位于 per-index meta block 中，用于将查询 term 定位到候选 DICT block。它是 DICT block 读取前使用的常驻元数据，随 SniiLogicalIndexReader 进入 searcher cache。
SampledTermIndex 的采样粒度是 DICT block，而不是固定 term 数。writer 每生成一个 DICT block，就把该 block 的 first_term 写入 SampledTermIndex。这样索引规模与 DICT block 数量成正比，而不是与 term 总数成正比。
SampledTermIndex 与 DICT block directory 配合使用：
- SampledTermIndex 保存有序的 block first_term，用于按 term 二分定位 block ordinal。
- DICT block directory 保存 block ordinal 到物理位置的映射，包括 offset、length、entry_count、flags、checksum 等。
- 二者按 block ordinal 对齐：`SampledTermIndex[i]` 对应 `DICT block directory[i]`。
结构：
```cpp
DICT block directory:
  block_ref[block_id]:
    offset
    length
    n_entries
    flags
    checksum

SampledTermIndex:
  n_blocks
  min_term
  max_term
  sample_terms[n_blocks]     # sample_terms[i] = first_term of DICT block i
  sample_term_offsets[]      # 指向 sample term blob
  sample_term_blob           # UTF-8 term bytes，可前缀压缩
```

exact term 查询流程：
1. 可选先查 XF。如果 XF 判断 term 一定不存在，直接返回空结果。
2. 在 SampledTermIndex 上二分，找到最后一个 `sample_term <= target_term` 的 block ordinal。
3. 如果 target_term 小于 `min_term` 或大于 `max_term`，直接返回不存在。
4. 用 block ordinal 到 DICT block directory 中取出该 block 的 offset / length。
5. 对该 DICT block 发起一次 range read，读入内存并校验。
6. 在 DICT block 内通过词项锚点和局部扫描查找 DictEntry。
7. 命中后由 DictEntry 生成 `.frq/.prx/norms` 的后续读取 range。
### 总结词典查询流程
```cpp
1. 常驻/缓存 meta
   用 sampled index、first_term / last_term、block offset / length
   定位候选 DICT block

2. 读取 DICT block
   对 [dict_block_offset, dict_block_offset + dict_block_len) 发起一次 range read
   读到内存后校验 crc32c
   在 block 内用 anchor_offsets 查找 term

3. 生成 postings 读取计划
   命中 term 后，从 DictEntry 得到：
   - inline posting，直接解码
   - 或 .frq offset / length
   - 如果需要 phrase，再得到 .prx offset / length
   - 如果需要 scoring，再关联 norms / stats

4. BatchRangeFetcher 读取 payload
   把多个 term、多列或多段产生的 .frq/.prx/norms ranges 合并后并发读取
```

## 倒排表设计
### 总体布局
```plaintext
.frq POD:
  per-term [optional columnar prelude][frq window 0..N-1]
  保存 doc delta + freq

.prx POD:
  per-term [prx window 0..M-1]
  保存 positions

norms POD:
  per logical index / field 保存 1B/doc encoded doc length
```

doc/freq 与 positions 分离存储。bitmap 查询只需要 doc；scoring 查询同时消费 doc 与 freq，因此 doc 和 freq 保持同流同窗。positions 只在 phrase / proximity 查询中读取，不与 doc/freq 交错。
### `frq` 设计
SNII 继承自适应窗口方向：以 256-doc 为基准 unit，组合成 256 / 512 / 1024 / 2048 doc 的窗口。**dd 与 freq 分离到 posting 级**（而非窗口内 `[dd][fq]` 交错）——windowed posting 的 `.frq` payload 布局为：
```plaintext
[prelude][dd-block][freq-block]
  dd-block   = 各窗口 dd_region 依次拼接（doc-delta，PFOR 位打包）
  freq-block = 各窗口 freq_region 依次拼接（freq，PFOR 位打包）
```

这样 **docid-only term 与所有短语只读连续的 dd-block、整段跳过 freq-block** —— 三指标同时降（read_at 不碎片化、range_gets 降、字节降），1MB FileCache 块对齐与直连 S3 下都省（若 dd/freq 共块，块对齐后跳 freq 等于没跳，故必须分离到 posting 级）。dd/freq 已 PFOR 位打包、熵接近下限，**默认 raw 存储**（zstd 仅在极少数可压窗口由 win_mode 位启用）。

窗口基准对齐使 merge stitch 的 byte-copy 通道可行：
```plaintext
win_base(0) = 0
win_base(w) = win_last(w - 1), w > 0

window payload:
  dd[0] = first_docid - win_base
  dd[i] = docid[i] - docid[i - 1], i > 0
```

非首窗 payload 不保存绝对 docid。segment merge 时，窗口内部 docid 相对关系不变者（除首窗或跨 run 边界窗口外）可以 byte-copy；绝对 docid 锚点、窗口长度和 checksum 修正由 prelude 的列式目录提供。

每窗的 region 元信息**不再放在 per-window header，而是上提到 prelude 列**（见下「窗口元信息 prelude」），每行含 `win_mode`（dd_zstd / freq_zstd 位）、`dd_off` / `dd_disk_len` / `crc_dd`、`freq_off` / `freq_disk_len` / `crc_freq`。reader 据此把命中窗口的 dd（scoring 时再加 freq）子段从 dd-block / freq-block 切出并 coalesce 成可并发 range。**inline DictEntry** 落在 dict block 内、已被 block 级 crc 覆盖，故**省去自身的 dd/freq region crc**（`pod_ref` 项保留——其字节在独立 `.frq` POD、不被 block crc 覆盖）。
### 窗口元信息prelude
当 `DictEntry.flags.enc = windowed `时，`.frq` payload 使用：
```plaintext
[prelude][frq window 0][frq window 1]...
```

prelude 是 windowed posting payload 的前置目录区，字节属于 `.frq` payload 域。DictEntry 保存 `prelude_len`，使 reader 第一轮读取计划可以精确加入 prelude range：
```plaintext
  prelude_start = frq_off
  window_start  = frq_off + prelude_len
```

```plaintext
prelude:
  header:
    u8   ver
    u8   flags(has_freq, has_prx)
    VInt N      # .frq window count
    VInt M      # .prx window count, has_prx 时存在
    VInt col_len[]
    u32  crc32c # header + all columns

  B  column: max_freq[N]
  B2 column: max_norm[N]
  C  column: last_docid_delta[N]
  D  column: win_mode[N] / dd_off[N] / dd_disk_len[N] / crc_dd[N]   # dd-block region 定位+校验
  D2 column: freq_off[N] / freq_disk_len[N] / crc_freq[N]           # freq-block region（has_freq 时）
  E  column: prx_cum_off[M]
  SB column: optional super-block directory
```

prelude 提供三类能力：
- 查询计划：通过 C / D / E 列定位 `.frq` 和 `.prx` 窗口，生成可合并、可并发的 range。
- 剪枝：通过 B/B2 计算窗口级 score upper bound。
- 多个 segment / spill run 合并：按列处理 run 边界重基、长度修正和 crc 重算。
补充对比CLucene skiplist和SNII的prelude
```sql
CLucene skiplist:
  目标是本地/文件流上 skipTo，加速顺序解码
  reader 已经打开 .frq/.prx stream
  通过 skipPointer 读 skip data，然后 seek 到 freq/prox pointer
  更偏执行期 cursor 跳转

SNII prelude:
  目标是S3 range 读取计划
  reader 可以先读一个很小的 prelude
  prelude 暴露 window / sub-block 的 docid 范围、offset、length、max score 等信息
  然后决定哪些 .frq/.prx 子 range 要加入 BatchRangeFetcher
  更偏 I/O 规划和批量并发读取
```

### `prx` 设计
`.prx` 独立分窗，并与 `.frq` 在 256-doc unit 边界上保持可对齐。具备 windowed positions 和 lazy `.prx` reader 的方向，同时包含自描述 header 与 checksum。
```plaintext
.prx window:
  u8   codec        # bit0-5: raw=0 / zstd=1 / pfor=2; bit7 cont-reserved
  VInt uncomp_len
  VInt comp_len     # codec == zstd
  u32  crc32c
  bytes payload     # codec 决定布局（见下）
```

`.prx` 默认编码为 **PFOR**（`codec=2`）：payload = `doc_count` ++ 每-doc `pos_count` 列 PFOR 位打包 ++ doc 内 position-delta 流 PFOR 位打包。位打包 `pos_count` 列（绝大多数为 1，约 1 bit/doc）是 PFOR 总体积反超 zstd 的关键——若 `pos_count` 用朴素 VInt，PFOR 体积反而比 zstd 大。相比 zstd，PFOR 无熵编码、构建 CPU 大幅更低，而位置 delta 单调升序使其体积持平甚至更优。`zstd`/`raw` 仍保留（强制路径与回放）。遇到超大单 doc positions 时，先退化为单个大窗口读取。
###  slim / inline 形态
`slim` 是 **小倒排表的紧凑编码形态**，面向低 df term。
小 postings 不进入 windowed 路径：
- df < 512：slim，使用紧凑 VInt 编码，无 prelude。
- slim && encoded_bytes <= inline_threshold：inline，posting 全部写入 DictEntry。
- df < 512 && encoded_bytes > inline_threshold：pod_ref slim，posting 写入外部 `.frq/.prx` range，由 DictEntry 记录 offset / length。
inline 用于减少低频 term 的额外 payload 读取；windowed 用于让中高 df term 在命中后可以先读 prelude，再生成 `.frq/.prx `的批量 range 读取计划。
## footer元信息
### 设计原则
Doris BE 可能同时打开大量 segment。SNII 的 tail meta region 必须保持紧凑，只保存查询规划所需的元数据。
tail meta region 的职责是让 reader 在打开 segment 后，能够快速完成以下动作：
- 校验 SNII container 与 meta region。
- 根据 (index_id, index_suffix) 找到对应 per-index meta block。
- 通过 per-index meta 定位 DICT block、`.frq` POD、`.prx` POD、norms POD 和 null bitmap POD。
- 在不读取 DICT block 和 postings payload 的情况下，完成基础统计读取、absent term过滤计划准备。
- norms 和 null bitmap 物理上可以靠近 tail meta region，便于 scoring 或 NULL 处理时做连续 range 读取，但它们不是 tail meta 的常驻内容。普通 bitmap 查询不应因为打开 index reader 就加载 raw norms。
### footer meta region 布局
```plaintext
tail meta region:
  TailMetaHeader
  per-index meta block #1
  per-index meta block #2
  ...
  logical index directory
  meta_region_checksum

TailMetaHeader:
  meta_format_version
  flags
  meta_region_len
  n_logical_indexes
  crc32c

logical index directory:
  entries[]:
    index_id
    index_suffix
    per_index_meta_off
    per_index_meta_len
    crc32c
```

### Per-index meta block
每个 logical index 对应一个 per-index meta block。该 block 随 `SniiLogicalIndexReader` 进入 searcher cache。
```plaintext
per-index meta block:
  PerIndexMetaHeader
  StatsBlock
  SampledTermIndex
  DICT block directory
  optional XF
  SectionRefs
  feature bits
  checksums

PerIndexMetaHeader:
  meta_format_version
  index_id
  index_suffix
  flags
  meta_len
  crc32c

StatsBlock:
  doc_count
  indexed_doc_count
  term_count
  sum_total_term_freq
  null_count
  crc32c

SectionRefs:
  dict_region_ref
  frq_pod_ref
  prx_pod_ref
  norms_ref
  null_bitmap_ref
```

StatsBlock 只保存统计信息；SectionRefs 统一保存各 data section / side POD 的物理引用，避免 stats 与 section 定位信息混在一起。
### footer元信息内存常驻与缓存
- tail meta region 通过 hot_off 定位，打开时按连续 range 读取。
- logical index directory 和 per-index meta block 属于查询规划元数据。
- SniiLogicalIndexReader 持有对应 per-index meta，并进入 searcher cache。
- searcher cache 由 LRU 统一淘汰。
- DICT block、WindowMeta、`.frq/.prx` payload、norms 和 null bitmap 不属于常驻 meta，按需通过 FileCache 读取。
- tail meta 和 DICT block 可标记为 index data；`.frq/.prx/norms/null bitmap` payload 走 normal data 路径。
## Scoring 统计设计
### 写入
scoring 需要在写入期补齐 BM25 所需的 collection / field / term / doc 级统计。当前 V4 路径主要服务 bitmap 查询，统计信息和 norms 不足以支撑原生 scoring evaluator。SNII writer 必须在 token 聚合、final merge 和窗口 flush 时生成完整统计。

<lark-table rows="9" cols="3" header-row="true" column-widths="191,191,405">

  <lark-tr>
    <lark-td>
      统计
    </lark-td>
    <lark-td>
      粒度
    </lark-td>
    <lark-td>
      用途
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      doc_count
    </lark-td>
    <lark-td>
      segment
    </lark-td>
    <lark-td>
      BM25 doc count、segment 边界
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      indexed_doc_count
    </lark-td>
    <lark-td>
      field
    </lark-td>
    <lark-td>
      空值、not indexed 与 NULL 处理
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      sum_total_term_freq
    </lark-td>
    <lark-td>
      field
    </lark-td>
    <lark-td>
      `avgdl = total_tf / doc_count`
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      df
    </lark-td>
    <lark-td>
      term
    </lark-td>
    <lark-td>
      idf、lead term 选择
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      total_term_freq
    </lark-td>
    <lark-td>
      term
    </lark-td>
    <lark-td>
      phrase/scoring 统计
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      encoded_norm
    </lark-td>
    <lark-td>
      doc/field
    </lark-td>
    <lark-td>
      BM25 length normalization
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      max_freq
    </lark-td>
    <lark-td>
      window/super-block
    </lark-td>
    <lark-td>
      block max score
    </lark-td>
  </lark-tr>
  <lark-tr>
    <lark-td>
      max_norm
    </lark-td>
    <lark-td>
      window/super-block
    </lark-td>
    <lark-td>
      block max score
    </lark-td>
  </lark-tr>
</lark-table>

说明：
- `doc_count`表示 segment 总行数。
- `indexed_doc_count``表示该 field 实际参与索引的 doc 数，应排除 NULL / ignored / not indexed doc
- `sum_total_term_freq`` 表示该 field 中所有 indexed doc 的 token 总数。
- `avgdl`应优先使用 `sum_total_term_freq / indexed_doc_count`，避免 NULL 或未索引 doc 稀释字段平均长度。
- `df` 与 `total_term_freq``写入 DictEntry 或 term stats 区。
- `encoded_norm` 写入 norms POD，按 doc / field 保存 1B encoded document length。
- `max_freq / max_norm`写入 WindowMeta，用于窗口级 score upper bound。
SNII writer 需要在文档 token 聚合时记录 doc length，在 term 聚合时记录 df / total_term_freq，在窗口 flush 时记录窗口级 max_freq / max_norm。
### SniiStatsProvider
```plaintext
SniiStatsProvider:
  doc_count()
  indexed_doc_count(field)
  sum_total_term_freq(field)
  doc_freq(field, term)
  total_term_freq(field, term)
  avgdl(field)
  encoded_norm(field, docid)
```

segment 内统计来源：
- `doc_count / indexed_doc_count / sum_total_term_freq` 来自 per-index meta 的 StatsBlock
- `doc_freq / total_term_freq`来自 DictEntry。
- `encoded_norm`来自 norms POD。
- `max_freq / max_norm` 来自 WindowMeta。
跨 segment / rowset 的 CollectionStatistics 由 SNII meta 与 DictEntry 聚合：
```cpp
global_doc_count =
  sum(segment.indexed_doc_count[field])

global_total_term_freq[field] =
  sum(segment.sum_total_term_freq[field])

global_doc_freq[field, term] =
  sum(segment.doc_freq[field, term])

idf =
  log(1 + (global_doc_count - global_doc_freq + 0.5) /
          (global_doc_freq + 0.5))

avgdl =
  global_total_term_freq[field] / global_doc_count
```

这里的 `global_doc_count`对 BM25 更准确地说是 field 级 indexed doc count，而不是 segment 总行数。segment 总 `doc_count` 仍保留用于边界、NULL 语义和调试统计。
如果 logical index 的配置不包含 scoring 所需字段，例如 docs-only / no norms / no total_term_freq，则该 segment 不参与 scoring evaluator，只能走 bitmap 查询或返回不支持 scoring 的错误。
### 窗口级 score upper bound
```cpp
windowed posting 在 WindowMeta 中保存窗口级统计：

WindowMeta scoring columns:
  max_freq[N]
  max_norm[N]

reader 可以用它们计算窗口级 BM25 score upper bound。对于 topK / WAND 类 evaluator，窗口上界用于决定哪些窗口需要读取完整 postings，哪些窗口可以跳过。

窗口上界是保守剪枝信息：

- `max_freq` 是窗口内真实最大 term frequency。
- `max_norm` 对应窗口内可产生最大 BM25 分数的 norm 编码值。
- evaluator 必须保证上界不低估真实最高分，否则会产生错误剪枝。
- 如果缺少窗口级统计，reader 不能做窗口级 scoring prune，只能读取对应窗口并完整计算。
```

## 


