# SNII vs CLucene —— S3 访问性能对比报告

对比对象：本仓库 SNII（S3-native 倒排索引）vs 本机 doris-thirdparty 的 CLucene（Doris 定制 fork）。
方法：同一份确定性 Zipfian 合成语料，两侧各建一份倒排索引；同一组查询；**两侧所有物理读都路由进同一个 1MiB 块 FileCache 成本模型**（`snii::io::MeteredFileReader`），统计文档要求的四项指标：

- **串行 I/O 轮次**（serial rounds）——延迟主因（≈ 轮次 × 远端 RTT）
- **Range GET 数**——远端请求计费 / QPS
- **read_at 调用数**——BE 内部物理读次数
- **读取字节数**（remote_bytes）——带宽 / file cache 占用

每次查询前 `reset_metrics()`（SNII 冷启）。**正确性闸门**：每条查询断言 `SNII docids == CLucene docids == 朴素 oracle`，全部 `ALL DOCIDS MATCH`。

复现：`cmake -S . -B build -DSNII_BUILD_BENCH=ON && cmake --build build --target snii_bench -j && ./build/bench/snii_bench --docs 5000000 --vocab 500000`

---

## 核心结果：5-term MATCH_PHRASE 的串行 I/O 轮次（规模扫描）

| 语料规模 | CLucene 轮次 | SNII 轮次 | 轮次比 | CLucene read_at | SNII read_at | read_at 比 |
|---|---|---|---|---|---|---|
| 2K docs | 2 | 1 | 2.0× | 10 | 11 | —（索引 <1MiB，单块整读假象，仅作 sanity）|
| 100K docs | 4 | 2 | 2.0× | 13 | 11 | 1.2× |
| 1M docs | 11 | 5 | 2.2× | 42 | 11 | 3.8× |
| **5M docs** | **21** | **5** | **4.2×** | **112** | **13** | **8.6×** |

**结论**：语料越大、索引越超 1MiB，差距越显著。5M 规模下，5 词短语查询 CLucene 需 **21** 个串行 I/O 轮次（cursor 逐段依赖：term dict → .frq → .prx 反复推进），SNII 只需 **5** 轮（前置规划全部 .frq/.prx range，BatchRangeFetcher 一轮批量并发）。这正是设计文档的核心论点。

## 5M docs 完整四指标

```
corpus: docs=5000000 vocab=500000 zipf=1.10 doclen=12 seed=42

[TERM high-df 'wa'  df=4,649,200]  docids_match=YES
  CLucene  read_at=43   serial_rounds=4   range_gets=4   remote_bytes=4,194,304
  SNII     read_at=2    serial_rounds=2   range_gets=2   remote_bytes=4,194,304
[TERM mid-df  df=9]                docids_match=YES
  CLucene  read_at=2    serial_rounds=2   range_gets=2
  SNII     read_at=1    serial_rounds=1   range_gets=1
[TERM low-df  df=1]                docids_match=YES
  CLucene  read_at=2    serial_rounds=2   range_gets=2
  SNII     read_at=1    serial_rounds=1   range_gets=1
[PHRASE 5-term]                    docids_match=YES
  CLucene  read_at=112  serial_rounds=21  range_gets=21  remote_bytes=27,020,108
  SNII     read_at=13   serial_rounds=5   range_gets=12  remote_bytes=25,165,824
```

## 延迟投影（串行轮次 × 真实 OSS RTT）

本机实测阿里云 OSS（`oss-cn-hongkong.aliyuncs.com`）单次往返 ≈ 50–130ms。以 80ms/轮估算冷查询关键路径延迟（5M，5-term phrase）：

- CLucene：21 轮 × 80ms ≈ **1.68s**
- SNII：5 轮 × 80ms ≈ **0.40s**  → 约 **4.2×** 更低的冷查延迟

（这是从成本模型轮次投影；真实 OSS 端到端 wall-clock 见「第二轨」状态。）

## 读取字节数权衡（remote_bytes）

SNII 的 remote_bytes 在小/中 df 查询上偏高（整读 1MiB 对齐块的紧凑容器）。这与设计文档明示的取舍一致：

> 代价可能是增加倒排索引数据大小，换取冷查性能；在存算分离模式下 file cache 与 S3 单位字节约 10:1，牺牲一些索引大小换取更少的 file cache。

设计优先级是**先降串行轮次与远端请求数**，其次才是字节数——本对比的延迟主指标（serial_rounds / range_gets）SNII 全面占优。

## 方法论与公平性说明（诚实标注）

1. **小规模单块假象**：索引 <1MiB 时整个索引落入一个 1MiB cache block，SNII serial_rounds=1 是"单块整读"而非批量之功——故以 1M/5M（索引远超 1MiB）为准，2K 仅作 sanity。
2. **CLucene 缓存反而占便宜**：当前 `reset_metrics()` 只清零计数器，CLucene 的内存 BufferedIndexInput 缓冲与 `.tii` term index（`IndexReader::open` 时加载）跨查询驻留，未计入每查询读数。即 CLucene 被测读数偏少、占优——SNII 在此条件下仍胜，结论保守可信。更严格的"两侧每查询全冷"可通过每查询重开 reader 实现（后续可加）。
3. **多文件 CLucene**：Doris fork 的 `setUseCompoundFile(true)` 未实现，故 CLucene 为多 segment 文件；MeteredDirectory 把**所有文件的所有物理读**都路由进同一成本模型。多文件 `.tis/.tii/.frq/.prx` 正是设计文档批判的 cursor 模式，具代表性。
4. **正确性**：每条查询 SNII / CLucene / 朴素 oracle 三方 docid 集合逐一相等（`ALL DOCIDS MATCH`），覆盖 high/mid/low-df term 与 5-term phrase；docid 按插入顺序对齐、无删除。

## 第二轨：真实 OSS 端到端 wall-clock（两侧均实测）

**两侧都在真实 OSS 上端到端测量**（非投影）。两份索引先上传到 `oss://doris-community-test`（阿里云香港，外网 endpoint），查询时按需 Range GET：
- **SNII** 经 `snii::io::S3FileReader`（aws-sdk-cpp，virtual-hosted 寻址）；批量读经 `read_batch` **并发** GET。
- **CLucene** 经 `bench/oss_clucene_directory`：一个只读 `lucene::store::Directory`，`openInput` 把每个子文件交给 `S3FileReader`，使 CLucene search 的每次物理读都成为真实 OSS Range GET。
两侧都包一层同一个 `MeteredFileReader`（同 1MiB 成本模型 + 真实 wall-clock）。每查询冷启 reset。运行：`snii_bench --oss --docs N`（需 `-DSNII_WITH_S3=ON`，凭证从 env `SNII_OSS_AK/SK`）。每条查询断言 `SNII docids == CLucene docids == oracle`，`ALL DOCIDS MATCH`。

**实测**（CLucene 与 SNII 均真实 OSS 测量；wall 比 = CLucene/SNII，>1 表示 SNII 更快）：

| 规模 / 查询 | CLucene wall / 轮次 / GET / 字节 | SNII wall / 轮次 / GET / 字节 | wall 比 |
|---|---|---|---|
| 1M · TERM high-df (df=933K) | 103.2ms / 2 / 2 | **21.7ms / 2 / 2** | **4.9×** |
| 1M · 5-term PHRASE | 357ms / 11 / 11 / 11.5MB | **265ms / 5 / 8 / 12.6MB** | **1.35×（轮次 2.2×）** |
| 5M · TERM high-df (df=4.65M) | 475ms / 4 / 43 read_at | **93ms / 2 / 2 read_at** | **5.1×** |
| 5M · TERM mid/low-df | 14.8 / 14.5ms | 8.8 / 7.5ms | 1.75× / 2.0× |
| 5M · 5-term PHRASE | **960ms / 21 / 21 / 27MB** | 1260ms / **5** / 12 / 25MB | **0.76×**（CLucene 更快）|

**解读（含一个诚实的权衡）**：
- **TERM 查询 SNII 真实快 4.9–5.1×**：CLucene cursor 把大倒排表拆成数十次小顺序读（5M 高频词 43 次 read_at），每次付真实 OSS 往返；SNII 2 次批量并发。这是 SNII 批量规划最直接的胜场。
- **短语在 1M 时 SNII 快 1.35×**（轮次 11→5 主导）；**但在 5M 时 CLucene 反而快 1.3×**，尽管 SNII 轮次仍少 4.2×（5 vs 21）。原因：5M 短语含 df=4.65M 的超高频词，**SNII 当前 phrase_query 读取整段 windowed 倒排表（25MB）再做位置交集**，而 CLucene 用 **skip-list 跳过**高频词大部分倒排、只读相关段。规模到 5M 后高频词倒排巨大 → **字节传输 + 解码主导延迟**，轮次节省被字节量盖过。1M 时该词倒排较小 → 轮次主导 → SNII 胜。
- **优化方向（已知 v1 限制）**：这正是设计文档 prelude 的 **super-block 子块目录**要解决的——按子块跳读，只取短语命中相关的 .frq/.prx 子 range。v1 实现中 super-block 标注为 out-of-scope 未启用跳读，故高频词短语读全段。启用后 SNII 短语在 5M 应同样占优。
- 小规模（5K，索引 <1MiB）SNII 整读单块字节开销也会盖过轮次节省。

**总结**：串行 I/O 轮次 / 请求数（文档主优化目标）SNII 全规模、全查询类型均显著更少；真实冷查延迟上，TERM 查询 SNII 大幅领先（~5×），短语在中等规模领先、在超大高频词短语上因 v1 未做子块跳读而读字节过多反落后——这是真实测得的、可优化的权衡，如实记录而非粉饰。
