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

## 第二轨：真实 OSS 端到端（状态）

确定性成本模型轨已完成（本报告）。真实 OSS 轨（aws-sdk `S3FileReader` 实现 `snii::io::FileReader` → 上传两份索引到 `oss://doris-community-test/cloud_regression` → Range GET 回读测 wall-clock）为后续可选增量，用于以真实延迟验证上面的"轮次 × RTT"投影。
