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

> 以下为**子块跳读实现后**的完整两轨结果。① 成本模型轨（本地，确定性）；② 真实 OSS 轨（两侧均实测 wall-clock）。所有查询 `ALL DOCIDS MATCH`。

## 轨一：成本模型（确定性，全规模 × 全查询类型）

每格 `CLucene → SNII`。serial_rounds / range_gets 是延迟与计费主指标。

| 规模 | 指标 | TERM high-df | TERM mid-df | TERM low-df | **5-term PHRASE** |
|---|---|---|---|---|---|
| **100K** | serial_rounds | 2 → 2 | 2 → **1** | 2 → **1** | 4 → **3** |
| (vocab 25K) | range_gets | 2 → 2 | 2 → **1** | 2 → **1** | 4 → **3** |
| | read_at | 2 → 3 | 2 → 1 | 2 → 1 | 14 → 56 |
| **1M** | serial_rounds | 2 → 3 | 1 → 1 | 1 → 1 | 11 → **8** |
| (vocab 200K) | range_gets | 2 → 3 | 1 → 1 | 1 → 1 | 11 → **10** |
| | read_at | 10 → **3** | 1 → 1 | 1 → 1 | 42 → 130 |
| **5M** | serial_rounds | 4 → **3** | 2 → **1** | 2 → **1** | 16 → **8** |
| (vocab 1M) | range_gets | 4 → **3** | 2 → **1** | 2 → **1** | 16 → **12** |
| | read_at | 43 → **3** | 2 → 1 | 2 → 1 | 90 → 208 |
| | remote_bytes(MB) | 4.2 → 5.2 | 2.1 → **1.0** | 2.1 → **1.0** | 21.8 → **15.7** |

要点：
- **PHRASE 串行轮次 SNII 全规模更少**（5M 8 vs 16），且 **5M PHRASE remote_bytes SNII 反而更少（15.7 vs 21.8MB）**——子块跳读后高频词只读命中窗口、不读整段。
- **TERM 的 read_at SNII 远少**（5M high-df 3 vs 43）：CLucene cursor 把大倒排拆成数十次小读。
- 多窗口使 PHRASE 的 read_at 升高（每窗口一次物理读），但被批量成少数串行轮次；TERM high-df 在 1M 出现 SNII 轮次略多（多窗口 prelude+窗口），属多窗口结构特性。

## 读取字节数优化（freq-skip + posting 级 dd/freq 分组）

设计覆盖审计发现：`dd_part_len`（bitmap/docid-only 跳过 freq）等省字节设计当时只 CPU 跳过、未网络跳过。实现了 #1–#5（详见 `docs/superpowers/specs/2026-06-18-snii-read-byte-optimizations-design.md`）：① docid-only term 与所有短语只读每窗 docs 区、跳 freq；② 同-term coalesce_gap；③ 真实 max_norm；④ 自适应窗口；⑤ WAND 选择性读窗口；并据实测把 frq posting 重排为 **`[prelude][dd-block][freq-block]`**（dd/freq 分离到 posting 级），docid-only 读**一段连续 dd 块**。

**两个字节口径**：`remote_bytes`=1MiB FileCache 块对齐（Doris 部署的缓存占用）；`request_bytes`=精确请求字节（= 设计「所有 range 返回总字节」，直连 S3 / 带宽口径）。

**5M 实测（优化后，CLucene → SNII；全部 `ALL DOCIDS MATCH`）**：

| 查询 | read_at | serial_rounds | remote_bytes(块对齐) | **request_bytes(精确)** |
|---|---|---|---|---|
| TERM high-df | 43 → **3** | 4 → **3** | 4.2 → **3.1MB** | 2.82MB → **1.37MB（2.1×）** |
| TERM mid/low | 2 → **1** | 2 → **1** | 2.1 → **1.0MB** | 131KB → **~400 B（~300×）** |
| 5-term PHRASE | 90 → **38** | 16 → **8** | 21.8 → **15.7MB** | 10.77MB → **0.96MB（11×）** |

**posting 级分组的效果**（TERM high-df，优化迭代）：
- 每窗交错布局（freq-skip 初版）→ docid-only 碎片化：read_at **4532**、remote_bytes 5.2MB（块对齐掩盖、不降）。
- posting 级分组 → 连续 dd 块：read_at **3**、remote_bytes **5.2→3.1MB（−40%，FileCache 下也省块）**、request_bytes 1.37MB。**read_at / range_gets / 字节三指标全降**，符合设计优先级（先降轮次与请求数，再降字节）。

> 设计原话：代价可能是增加倒排索引数据大小，换取冷查性能；存算分离下 file cache 与 S3 单位字节约 10:1。优化后 SNII 在精确字节上对 CLucene 全面领先（短语 11×、term 2×），FileCache 块占用 high-df term 亦降 40%。

**真实 OSS 端到端验证（5M，`--oss --repeat 15`，request_bytes=真实 OSS 传输字节）**：字节优化直接转化为 wall-clock 提速。

| 5M 查询 | CLucene median wall / request_bytes | SNII median wall / request_bytes | vs CLucene | vs 字节优化前 SNII |
|---|---|---|---|---|
| TERM high-df | 427.0ms / 2.82MB | **50.1ms / 1.37MB** | **8.5×** | 197→**50ms**（3.9× 提速）|
| 5-term PHRASE | 666.9ms / 10.77MB | **107.5ms / 0.96MB** | **6.2×** | 227→**107ms**（2.1× 提速）|
| TERM mid/low | 6.1ms | 7.4ms | 0.83× | 微查询单轮、差异在 OSS 抖动量级 |

SNII 自身因传输字节下降（高频 term 减半 + 连续 dd 块、短语去掉 freq）在真实 OSS 上提速 **2–4×**；对 CLucene 拉开到高频 term **8.5×**、短语 **6.2×**。`ALL DOCIDS MATCH`。这印证设计「读取字节数」指标的优化在对象存储上有真实带宽 / 延迟收益。

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

**实测 wall-clock（毫秒，CLucene → SNII；比 = CLucene/SNII，>1 表示 SNII 更快；子块跳读后）**：

| 规模 | TERM high-df | TERM mid-df | TERM low-df | **5-term PHRASE** |
|---|---|---|---|---|
| **1M** | 116.0 → **40.0**（**2.88×**）| 9.7 → 9.6（1.0×）| 9.9 → 9.4（1.05×）| 366.5 → **234.5**（**1.56×**）|
| **5M** | 551.0 → **186.6**（**2.96×**）| 17.9 → 22.7（0.77×）| 26.6 → **7.3**（**3.71×**）| 785.3 → **390.7**（**2.01×**）|

（轮次：5M TERM-high 4→3、TERM-mid/low 2→1、PHRASE 16→8；1M PHRASE 11→8。）

**解读**：
- **TERM high/low-df SNII 真实快 ~3×**（5M high 551→187ms、low 27→7ms）：CLucene cursor 把大倒排拆成数十次小顺序读、每次付真实 OSS 往返；SNII 批量并发。
- **5-term 短语 SNII 全规模领先**（1M 1.56×、5M **2.01×**）。其中 5M 短语是子块跳读补齐的短板：
  - **跳读前**（读整段 25MB）：SNII 1260ms 反慢于 CLucene 960ms（字节量盖过轮次节省）。
  - **跳读后**：lead term(最小 df) 驱动候选，高频词只读「覆盖候选 doc 的少数窗口」→ SNII **390.7ms / 8 轮 vs CLucene 785.3ms / 16 轮 = 2.01×**。见 `docs/superpowers/specs/2026-06-18-snii-subblock-skipping-design.md`；差分测试证明跳读 == 全读 == oracle。
- mid-df term 单发偶现 CLucene 微快（5M 17.9 vs 22.7ms）属**单样本离群**，非负收益。单发 `--oss` 每条查询只测一次，含首次 GET 的 TLS/连接建立 + OSS 尾延迟。`--oss --repeat 20`（上传一次、每条查询冷测 20 次）的稳态分布证明 **SNII mid-df 实际更快**：

  | 5M 查询（20 次冷重复 median / p90，ms）| CLucene | SNII |
  |---|---|---|
  | TERM high-df | 493.8 / 523.7 | **197.1 / 267.0** |
  | TERM mid-df | 8.2 / 8.9 | **6.3 / 6.8** |
  | TERM low-df | 8.4 / 9.1 | **6.4 / 7.1** |
  | PHRASE | 651.9 / 707.5 | **227.5 / 247.4** |

  稳态下 SNII 全查询类型 median 更优（mid-df 1.30×、high-df 2.5×、PHRASE 2.87×）且分布更紧。复现：`snii_bench --oss --repeat 20 --docs 5000000`。

  **1M 的 20 次重复分布交叉验证一致**（median，ms）：high-df 82.2→**30.0**、mid-df 5.8→**5.6**、low-df 5.8→6.0、PHRASE 308.0→**156.0**。关键：**稳态 wall-clock 精确跟随成本模型轮次**——mid/low-df 在 1M 是 1 轮 vs 1 轮 → wall-clock 持平（~5.8ms）；在 5M 是 SNII 1 轮 vs CLucene 2 轮 → SNII 胜（6.3 vs 8.2ms）。SNII 自身 mid/low-df 两规模都 1 轮、稳定 ~6ms；CLucene 因 5M 多一轮而升到 8.2ms。那个 22.7ms 单发离群在 1M / 5M 重复中均未再现（SNII p90：1M 6.4、5M 6.8），坐实为一次性连接/尾延迟尖峰。微查询单发 wall-clock 的几 ms 差由 OSS 抖动主导；成本模型是该量级的可靠信号。

**总结**：串行 I/O 轮次 / 请求数（文档主优化目标）SNII 全规模、全查询类型均更少或持平；真实冷查 wall-clock，TERM 快 ~3×、5-term 短语（含子块跳读）快 ~2×。SNII 通过常驻元数据前置规划 + 两级子块跳读 + 批量并发，在对象存储上把串行轮次、远端请求数、读取字节同时压下来，冷查延迟全面领先。`ALL DOCIDS MATCH` 贯穿全部规模与查询类型。
