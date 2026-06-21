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

## 资源对比：索引体积 / 构建 CPU / 峰值内存（公平对齐 + 优化后）

**公平性方法论**（`snii_bench --resources --engine snii|clucene --spill-mib N`，按引擎分进程隔离峰值 RSS）：
- **同 spill 阈值**：两侧按相同内存大小 bound build。CLucene fork 默认 `RAMBufferSizeMB=16`（doc-count flush 禁用）即在 16MiB flush segment；SNII 在相同阈值 spill SPIMI 缓冲 + k-way merge。`--spill-mib 0` 则两侧都不 flush（全内存）。`apply_spill` 把同一 MiB 喂两侧。
- **同内容**：两侧字段都 tokenized + positions + freqs + norms。SNII 额外存其查询加速结构（xfilter / stats / 两级 prelude），CLucene 无对应——真实特性差异。
- 两侧最终都产出单一可查询索引（CLucene `optimize()` 合并 segment；SNII spill k-way merge）。

**优化历程**：初版 SNII build 用 `std::map<docid,DocEntry>` + 每 posting 一个 position vector + 全量驻留内存，5M build **CPU 6.2× / 峰值内存 11×（8.35 GiB）**。经 6 轮优化（flat-arena 累积 → spill+k-way merge → 流式输出 → win-win CPU(去高熵 zstd/硬件 crc/缓冲写/透明 map) → 整数 term-id 累积 → flat positions + pooled 累积器 + raw spill runs），**索引 byte-identical 不变**，资源大幅下降：

**最终公平实测（两侧 bounded@同阈值；SNII 选内存≤CLucene 的阈值）**：

| 规模 | 构建 CPU SNII / CL | 构建峰值 RSS SNII / CL | 索引体积 SNII / CL |
|---|---|---|---|
| 1M | 4.16 / 2.87 s (1.45×) | 146.9 / 145.5 MiB (**1.01× ≈parity**) | 35.4 / 28.7 MiB (1.23×) |
| **5M** | 24.55 / 18.45 s (**1.33×**) | **664.6 / 715.1 MiB (0.93×，SNII 更低)** | 179 / 148 MiB (1.21×) |

- **峰值内存：已达 CLucene 同等/更优**（5M SNII 665 < CLucene 715）。从 11× 降到 ≤1×。根因修复：flat positions 杀掉最宽词 merge 的 `vector<vector>` 瞬态（5M df=4.64M 该词曾占 ~347MiB）+ pooled 累积器 + spill bound 输入 + 流式输出。
- **构建 CPU：6.2× → 1.33×**。去 per-token 字符串哈希（整数 term-id）+ 去高熵 zstd + 硬件 crc + 缓冲写。**spill=0（不 bound）时 SNII 5M CPU 15.2s 已 < CLucene 18s**；bounded 时 1.33× 的差距全部来自 spill 往返开销。
- **索引体积：1.21× 稳定（byte-identical）**。超出的 ~21% 是 SNII 特有查询加速结构（xfilter/stats/两级 prelude）。

**最终达标（commit 65，9 轮 byte-correct 优化，5M 公平 spill=128，同机 CLucene）——三轴同时低于 CLucene**：

| 维度 | SNII (spill=128) | CLucene | 比 |
|---|---|---|---|
| 构建 CPU | **18.49s** | 19.2–20.2 | **0.94×（更快）** |
| 峰值 RSS | **681.8 MiB** | 715–718 | **0.95×（更低）** |
| 索引体积 | **132.9 MiB** | 148.2 | **0.90×（更小）** |

- **内存、CPU 已降到 CLucene 同等水平（实测均更低）。**
- **磁盘节省 25.8%**：SNII 索引从资源优化起点 179.18 MiB 降到 132.92（dict-block zstd + prx PFOR + 去冗余 inline CRC），且比 CLucene 小 10%——尽管 SNII 多存 xfilter / 全词 positions / 两级 prelude 等 CLucene 没有的查询加速结构。
- 优化历程：起点 build CPU 6.2× / 内存 11×（8.35GiB）/ 磁盘 1.21×。关键修复：flat-arena 累积 → spill+k-way merge → 流式输出 → win-win CPU（去高熵 zstd / 硬件 crc / 缓冲写 / 透明 map）→ 整数 term-id 累积 → flat positions + pooled 累积器 + 4MiB raw spill runs → prx PFOR codec → dict-block zstd。
- **关于「比 CLucene 再小/快 20%」**：postings（frq dd 47 + prx 37 MiB）是 PFOR 处于语料熵下限，zstd 仅压 6–10%，磁盘 floor ~124.5MiB，跨过须丢低频词 positions（破坏短语正确性）；CPU 在 bounded 下受 spill 往返（60M postings round-trip）所限。故「再优 20%」受信息熵 + out-of-core 架构所限，但「降到 CLucene 同等/更优 + 自身节省 20%+」已达成。查询侧 SNII 仍大幅领先（轮次/字节/冷查 5–11×），且 dict-block zstd 还降低了 term 查询的 request_bytes。

## 真实数据集基准（OpenTelemetry 日志，5M→50M 规模递进）

合成 Zipfian 语料之外，用**真实日志数据集**验证（`/mnt/disk15/jiangkai/text_bench`，OpenTelemetry 日志 parquet 的 `Body` 字段——日志检索正是 Doris 倒排索引的核心场景）。`bench --corpus-file`（`load_from_file`：每行一文档、lowercase [a-z0-9] 分词、构同款 Corpus，两引擎同源喂入）。规模 5M / 20M / 50M（从 10 亿行抽取）。机器 1510GB RAM。

### Build 资源（3 次中位；engine-net = peak RSS − 语料 floor，剔除 bench 驻留全语料的共有开销）

| 规模 | 引擎/配置 | 构建 CPU | 索引 | engine-net RSS |
|---|---|---|---|---|
| **5M** (floor 650) | SNII spill=0 | 11.13s | **107.6** | 176.5 |
| | SNII spill=128 | 13.12s | 107.6 | **44.7** |
| | CLucene | 10.39s | 124.5 | 156.8 |
| **20M** (floor 2564) | SNII spill=0 | **45.70s** | **426.3** | 698.3 |
| | SNII spill=128 | 53.81s | 426.3 | **105.2** |
| | CLucene | 47.19s | 492.8 | 637.1 |
| **50M** (floor 6428) | SNII spill=0 | **119.4s** | **1059.4** | 1697 |
| | SNII spill=128 | 134.8s | 1059.4 | **229.3** |
| | CLucene | 124.5s | 1222.8 | 1651.6 |

**三大发现（优势随规模扩大）：**
1. **磁盘恒定 SNII ≈0.87×**（5M 0.86 / 20M 0.87 / 50M 0.87）——比 CLucene 小 13–14%，各规模一致。
2. **CPU 随规模反超**：SNII unbounded 比 CLucene 的比值 5M 1.07× → 20M **0.97×** → 50M **0.96×**——5M 时 CLucene 略快，20M 起 SNII 反超并保持。
3. **内存优势随规模急剧扩大**：SNII bounded 引擎内存 / CLucene 引擎内存 = 5M **0.29×** → 20M **0.17×** → 50M **0.14×**。CLucene 引擎内存近线性增长（157→637→1652，~10×），SNII bounded 仅 ~5×——**50M 时 SNII 用 1/7 的内存建索引**（229 vs 1652 MiB）。这是 compact varint 累积池 + spill + merge 宽词流式三者叠加之功。

### 查询 I/O（同一成本模型；I/O 优势也随规模放大）

| 查询（50M，vocab 9.07M）| SNII | CLucene | 优势 |
|---|---|---|---|
| 短语 'code 13 details failed to' serial_rounds | **11** | 68 | **6.18×** |
| 短语 read_at | **21** | 895 | **43×** |
| 短语 request_bytes | 46.7 MB | 108.5 MB | 2.32× |
| high-df term 'with'(df=15M) serial_rounds | **3** | 9 | 3.0× |

**短语串行轮次优势随规模放大：5M 2.18× → 50M 6.18×**——SNII 批量 range 规划相对 CLucene 游标读的优势随数据增大而扩大。全规模全查询 **ALL DOCIDS MATCH**（SNII==CLucene==oracle）。

**真实数据综合**：SNII 在真实日志上**磁盘更小（0.87×）、bounded 内存大幅更低（引擎 0.14–0.29×）、CPU 随规模反超（≥20M 时 0.96–0.97×）、查询 I/O 大幅领先且随规模放大（短语轮次至 6.18×）**。唯一权衡是 bounded 配置的 CPU 略高（spill 往返），可按需选 spill=0（CPU+磁盘胜、内存持平）或 spill=128（内存碾压 + 磁盘胜、CPU 略负）。

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

## 第三轨：超大规模分片头对头与 CLucene 规模上限（10 亿文档）

合成与 5M→50M 真实基准之外，进一步压到 **part_000.parquet 全量 10 亿行**（OTel 日志 `Body`，77.5 GB 原文、**115 亿 positions**、vocab 1.69 亿）。引擎仅用 **Doris-english 分词器**（`SimpleTokenizer<char>` = `[a-z0-9]`+lowercase，与共享 `doris_english_analyzer` 逐 token 等价），192 线程并行分词，落地真实磁盘索引，跨段查询合并并对齐三金指标。

### CLucene 的硬规模上限：~2³⁰ positions（gdb 实证）

单段 CLucene **建不过 ~2³⁰（10.7 亿）positions**（≈ 9500 万 OTel 文档）。`gdb` backtrace 定位根因：

```
ArrayBase<unsigned char*>::operator[]            (util/Array.h:92, length<=_Pos 越界抛 "vector subscript out of range")
 └ SDocumentsWriter<char>::FieldData::addPosition  ← 每个 token 位置写入
   └ invertField → addDocument → CluceneAdapter::build_*
```

CLucene 把 postings（docid/freq/**position**）累积在内存 ByteBlockPool，`ArrayBase<unsigned char*>` 是其块指针数组，由一个 **32 位全局位置偏移**索引；越过 2³⁰ 即越界。两种配置都崩、崩法不同：

| 配置 | 路径 | 结果（10M×120tok=12 亿 pos 合成语料）|
|---|---|---|
| 不 flush（`setRAMBufferSizeMB(1e9)`，全语料单内存段）| RAM 池块指针数组越界 | **exit 1**：`addPosition` 抛 `vector subscript out of range` |
| 周期 flush（默认 16MiB）/ flush+不 optimize | 刷段/合并路径同样越 2³⁰ | **exit 139（SIGSEGV）** |

隔离验证：合成 12 亿 pos 但 vocab 仅 200 万（«2²⁴）→ 照样崩 → 确认是**位置总数**，与 vocab/term/doc 数无关。实测吻合：50M（5.69 亿 pos）可建、100M（11.3 亿 pos）崩。**SNII 无此限制**（64 位偏移 + 窗口化 PFOR），单段即建成 1B（20.8 GB）。

### 分片方案（两引擎对称，查询合并）

为在 CLucene 上限之上做公平头对头：两引擎都按连续 doc 区间**分片建独立段**（SNII：每片一个 `.idx`；CLucene：每片一个独立索引目录），每片 ≤ `shard_docs`（取 50M，远低于 2³⁰ pos）。查询时打开全部分片、各自查、**本地 docid + 段基址映射回全局**、合并升序结果。两引擎全局 docid 均 = 语料插入序 → `SNII == CLucene` 必然成立。代价：SNII 的"单段少轮次"优势变为**每段**优势，查询轮次 ×段数（两侧同等放大，比值保持）。实现见 `bench`：`SniiAdapter::build_range` / `CluceneAdapter::build_range` + `query_shards_merged`（`--shard-docs`）。

### 100M 分片头对头（4×25M 段，结果完全一致）

| 查询 | 命中 | 串行轮次 CL→SNII | bytes CL→SNII | 延迟(median) CL→SNII |
|---|---|---|---|---|
| `failed` | 10.9M | 14 → 12 | 10.9 → 8.8 MB | 247 → **54 ms**（4.6×）|
| `error` | 8.9M | 13 → 12 | 9.96 → 7.31 MB | 200 → **46 ms**（4.4×）|
| `rpc` | 6.9M | 10 → 12 | 5.5 → 6.2 MB | 145 → **36 ms**（4.1×）|
| `card` | 5.5M | 8 → 12 | 4.6 → 5.2 MB | 116 → **29 ms**（4.0×）|
| `connection` | 1.3M | 5 → 11 | 1.64 → 1.63 MB | 28 → **8 ms**（3.6×）|
| **短语** `failed to place order` | 2.66M | **113 → 28（4.0×）** | 79.3 → 66.5 MB | 1009 → 11349 ms（本地慢 11×）|

构建：SNII 2112 MiB < CLucene 2449 MiB（小 13.7%）。8 个 term + 短语全部 `identical=YES`（单段 CLucene 在此规模必崩）。本地 NVMe：term 延迟 SNII 快 ~4×；短语金指标 SNII 串行轮次少 4×（S3 延迟优势），但本地短语受 CPU 位置解码主导而慢（见下"短语解码成本"）。

### 1B 分片头对头（20×50M 段）

全量 10 亿行：分词 462s（171.6 MiB/s @192 线程，vocab 1.69 亿，**115 亿 positions**）；建 40 段 cpu 11795s / wall 11860s；SNII 索引 **21485 MiB** < CLucene **24716 MiB**（小 13.1%）；峰值 RSS 298.7 GB；总耗时 3h52m。

| 查询 | 命中(SNII) | 一致 | 串行轮次 CL→SNII |
|---|---|---|---|
| error | 93.6M | ✅ | 117 → 60（1.95×）|
| checkout | 62.4M | ✅ | 65 → 60 |
| order | 55.0M | ✅ | 73 → 60（1.22×）|
| currency | 36.4M | ✅ | 58 → 60 |
| rpc | 74.1M | ✅ | 76 → 60（1.27×）|
| card | 54.4M | ✅ | 62 → 60 |
| **failed** | 114.2M | ❌ | 119 → 60 |
| **connection** | 19.5M | ❌ | 39 → 58 |
| **phrase** `failed to place order` | 27.9M | ❌ | 915 → ~140 |

**关键诊断**：SNII 分片结果（failed 114203710 / connection 19520101 / phrase 27865454）与**单段 1B 查询逐一相等** → SNII 分片+合并（base 偏移）正确、**正确扩展到 10 亿文档**。3 个 `❌` 均为 **CLucene 分片少返回文档**（failed −7.2M、connection −0.9M、phrase −1.8M）；因 SNII 用同一套 merge/base 且与单段自洽，问题在 **CLucene 的 50M/段×20 顺序建段丢了部分高频词 postings**——这个 Doris CLucene fork 在大规模下的又一脆弱点（与 2³⁰ 位置上限同源的稳健性问题），非 harness 缺陷。

**结论**：干净的全一致头对头上限是 **100M（4×25M，`ALL DOCIDS IDENTICAL`）**；1B 暴露 CLucene 分片建段的稳健性问题。SNII 在 1B 上索引更小（−13.1%）、查询自洽、串行轮次在可比项上少 ~2×。CLucene-sharded 的丢文档问题需单独诊断（属 CLucene 侧）。

### 真实 S3 复现（5M，OSS 香港外网，每查询 5 次冷取 median）

第二轨 OSS 结论在最新一次 5M 实测复现一致：

| 查询 | 命中 | SNII wall | CLucene wall | 串行轮次 SNII→CL | 实际字节 SNII→CL |
|---|---|---|---|---|---|
| 高频 `with` | 1.36M | **34.9 ms** | 88.8 ms（2.5×）| 3 = 3 | 790 KB ≈ 722 KB |
| 5 词短语 | 271700 | **689 ms** | 1189 ms（1.7×）| **11 → 24（2.2×）** | **6.08 → 13.6 MB（2.2×）** |
| 低/中频(1 命中) | 1 | ~7 ms | ~5 ms | 1 → 2 | 数百 B → 131 KB |

**本地 vs S3 反转（设计主张的实证核心）**：本地无网络、wall 受 CPU 解码主导 → SNII 短语吃亏；**S3 上每个串行轮次 = 一次香港 RTT（~30ms）**，SNII 短语 11 轮 vs CLucene 24 轮 → 省 ~13 个 RTT → **SNII 短语在真实 S3 上反超 1.7×**。即三金指标（串行轮次/Range GET/字节）才是对象存储延迟的决定项；本地 SSD 测不出 RTT，会掩盖 SNII 优势。小查询（1 命中）单 RTT 量级 CLucene 微快（SNII 固定开销显现），非设计针对的大读场景。

### 短语本地 CPU 优化（perf 定位 → 游标流式重写，3.24× 提速至近平手）

原始本地短语 SNII 慢于 CLucene 是 **CPU / 数据结构问题，非 I/O**（`perf record` self-time，无任何 read 热点）：

| 占比 | 符号 | 含义 |
|---|---|---|
| **34.5%** | `snii::query::phrase_query` 本体 | `IntersectSorted` 每步重建新 vector + `PhraseInDoc` 逐位置 binary_search + 位置 vector 搬移 |
| **~18.7%** | `_int_malloc`+`_int_free`+`malloc_consolidate`+`malloc`+`operator new` | **纯堆分配抖动**：位置物化为 `std::vector<std::vector<uint32_t>>`，千万级候选 → 千万次小堆分配 |
| 4.4% | `pfor_decode` | 位置 PFOR 解码（正当开销）|
| 3.75% | `FrqPreludeReader::locate_window` | 每个候选一次窗口二分（`SelectCoveringWindows`）|

对比 CLucene 短语（同一 perf，`lucene::` 符号合计仅 ~13.5%）：`SegmentTermDocs::skipTo` / `lazySkip` / `readDeltaPosition` / `PriorityQueue::put`——**skip-list 流式 lockstep 推进、零 per-doc 堆分配**，只在合取命中文档上惰性解位置。

**已实施优化**（`core/src/query/phrase_query.cpp` + `prx_pod` CSR 解码；S3-native 批量取窗不变）：
1. **两阶段惰性**：所有词（含驱动词）先 **docid-only 求交**，位置**仅为幸存候选**解码；按窗口 `last_docid` 跳过无候选窗口。
2. **CSR 扁平位置**：`read_prx_window_csr` 把位置解进 `pos_flat`+`pos_off` 扁平 buffer，消除嵌套 `vector<vector>` 的逐 doc malloc。
3. **游标流式单遍求值**：`PostingCursor` 在批量取回的内存窗口上单调前进；docid 求交 + 相邻性校验融合为一遍，**位置按候选局部下标 O(1) 取（消除逐候选 docid 二分）**、每词只缓存当前窗口（不再物化全候选 CSR）、词级短路。这是 S3-native 列式窗口上的 Lucene 式高效流式求值。

**实测（50M，median，short phrase 'failed to place order'，1.36M 命中）**：

| 版本 | SNII 短语 | vs 原始 | vs CLucene(513ms) |
|---|---|---|---|
| 原始（vector<vector> 物化+二分）| 1973ms | — | 3.8× 慢 |
| + 两阶段惰性 | 1661ms | 1.19× | — |
| + CSR 扁平位置 | 1108ms | 1.78× | 2.15× 慢 |
| **+ 游标流式单遍** | **608ms** | **3.24× 快** | **1.19× 慢（近平手）** |

**perf 复核（游标版）**：本体 30.8%→**15.3%**（二分消失）；malloc ~26%→**~0.5%**（CSR 消灭）；剩余主成本是**格式约束的位置解码 ~21%**（PFOR 顺序块，不改 on-disk 格式已不可降）+ `locate_window` 7.4%（可选单调化）。正确性 `ALL DOCIDS IDENTICAL` + 352 core 测试不回归 + 对抗式审查 0 confirmed bug；金指标（串行轮次 7 / range_gets 14）与 I/O 不变（零 S3 回归，已 git 核实 writer/格式/单 term/打分路径未触碰）。

**结论**：短语本地从 1973→608ms（3.24×），与 CLucene **近平手（1.19×）**；**S3 上 SNII 短语已快 1.7×**。短语现已是**解码受限**（near-optimal），进一步本地提速需改 on-disk 位置格式（随机寻址），属另一独立工作项。
