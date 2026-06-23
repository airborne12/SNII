# SNII E2E 真实数据集导入基准 设计规格

> 日期：2026-06-20 ｜ 分支：feat/snii-phase1 ｜ 状态：已实现（TDD），50k 冒烟通过 ALL DOCIDS MATCH

## 实现备注（落地与设计的偏差）

- 分词器一致性：经核实 Doris fork 的 `SimpleTokenizer<char>::isTokenChar` = `[a-zA-Z0-9]`（保留数字 + LowerCaseTokenizer 小写），与 `doris_english_normalize` 语义**完全一致**，故 CLucene 端无需改分词器，沿用 `SimpleAnalyzer<char>` 即逐 token 等价。
- 落盘可重开：`SniiAdapter` 新增 `open_existing(path)` 仅读重开持久化 .idx，E2E 末尾用全新 reader 重开校验同一 term 的 docid 一致。
- 峰值内存口径：`--engine both` 单进程同时建两引擎，峰值为整条流水线高水位；要干净的单引擎峰值需 `--engine snii` / `--engine clucene` 分进程运行（沿用既有方法论，输出已提示）。
- CLI：`--parquet-file` 触发 E2E 模式，配套 `--text-col`(默认 Body)/`--threads`(0=硬件并发)/`--out-dir`(默认 e2e_data/run)/`--keep-index`/`--no-keep-index`/`--runs`(中位)。

## 1. 目标（Why）

在**真实 parquet 数据集**上跑通单进程端到端链路，并在同一份分词输出上**公平对比 SNII 与 doris-thirdparty CLucene**：

```
parquet 原文(Body) → Doris-english 多线程分词 → 双引擎落盘真实索引
  → SNII↔CLucene 查询一致性校验 → 度量导入 CPU / 峰值内存 / 索引文件大小
```

与现有合成/玩具基准的差异：
- 数据源换成真实 parquet（`Body` 列），而非合成语料或行分隔 txt；
- 分词器升级为**单一权威 Doris-english 实现**，两引擎共用同一份 token（保证一致性）；
- 分词阶段**多线程**；
- 索引**持久化落盘**到可检视目录（非临时目录，运行后保留）。

## 2. 数据源（What·输入）

- 文件：`/mnt/disk15/jiangkai/text_bench/data/part_000.parquet`
- 规模：10 亿行 OTel 日志；schema 含 `Body`(string, 全文索引列, 均长 ~83B / 中位 59B / max 1350B)、`ServiceName`、`SeverityText` 等。
- 截取：`--docs N`（行序即 docid）。主轨先 **5M smoke**，再放大 20M / 50M。
- 依赖：Apache Arrow / Parquet 静态库与头文件位于 `/mnt/disk1/jiangkai/workspace/install/installed-master`（`lib64/libparquet.a`、`lib64/libarrow*.a`、`include/arrow`、`include/parquet`）。

## 3. 范围与边界

- 多线程**仅作用于分词**（已确认）。建段保持单线程单段，沿用现有单段 reader 与 `ALL DOCIDS MATCH` 一致性校验，不引入多段读取器。
- 一致性采用**「单源喂双引擎」**：Doris-english 实现一次，CLucene 接收预切 token（空格 join + WhitespaceAnalyzer），保证两引擎 token 集逐一相等。
  - 说明：仅保证**两引擎之间**一致；与 Doris 生产 english parser 的逐字节等同只做语义对齐（lowercase + 非 `[a-z0-9]` 切分 + 64 字节截断），不复刻其内部对下划线/连字符等的处理。

## 4. 数据流与组件（How）

### 4.1 数据流
```
part_000.parquet[Body]
  │ Arrow 流式读取(≤N)                 ← parquet_corpus_reader
  ▼ raw bodies (docid=行序)
  │ T 线程：阶段A 并行分词+线程局部词表  ← parallel_tokenizer
  │         阶段B 串行合并词表+id 重映射（确定性，与串行逐字节等价）
  ▼ bench::Corpus (vocab + docs[d]=term-id)   ← 双引擎唯一分词源
  ├─► SniiAdapter.build_at(out/snii/index.idx)
  └─► CluceneAdapter.build_at(out/clucene/)
  ▼ 落盘真实文件（保留，打印 ls -la，验证可被 reader 重开）
  │ 真实词表采样 term + phrase 查询
  ▼ ALL DOCIDS MATCH 一致性断言（首个 mismatch 报告）
  ▼ 三轴度量(多轮取中位) + 分词吞吐表
```

### 4.2 新增组件
| 文件 | 职责 |
|---|---|
| `bench/doris_english_analyzer.{h,cpp}` | Doris-english 语义权威实现：`doris_english_normalize(byte)->int`、`kDorisEnglishMaxTokenLen=64`、`doris_english_for_each_token(sv, emit)`、`doris_english_tokenize(sv, out*)`。纯函数，无状态。|
| `bench/parallel_tokenizer.{h,cpp}` | `tokenize_corpus(const std::vector<std::string>& bodies, uint32_t threads) -> Corpus`。阶段A：T 线程对 doc 区间无锁并行分词，各自维护**线程局部词表**（局部 id 按区间内首现序）+ 局部 `docs`。阶段B：按线程序 0..T-1、线程内局部 id 序合并出全局词表（全局 id 按 doc 序首现），再用 local→global id 数组重映射。结果与单线程逐字节等价。|
| `bench/parquet_corpus_reader.{h,cpp}` | `read_text_column(path, column, max_docs) -> std::vector<std::string>`。Arrow 流式读指定字符串列前 ≤N 行；缺列/打开失败抛 `std::runtime_error`。|

### 4.3 修改
| 文件 | 改动 |
|---|---|
| `bench/corpus_loader.cpp` | 复用 `doris_english_normalize`/`kDorisEnglishMaxTokenLen`（DRY），流式逻辑不变，现有 7 测试须仍通过。|
| `bench/snii_adapter.{h,cpp}` | 增 `build_at(const std::string& dir, const Corpus&)` 持久化到 `dir` 并打开；`std::vector<std::string> index_files()` 供 ls。`build_and_open` 改为委托临时目录。|
| `bench/clucene_adapter.{h,cpp}` | 同上：`build_at` + `index_files()`。|
| `bench/main.cpp` | 新增 E2E 模式与开关：`--parquet-file`、`--text-col`(默认 Body)、`--threads`、`--out-dir`、`--keep-index`、`--runs`。串起读取→并行分词→双引擎落盘→一致性→度量→打印 ls。|
| `bench/CMakeLists.txt` | 仅 `snii_bench` 链接 Arrow/Parquet（installed-master 树）；新增 analyzer/tokenizer/parquet_reader 三个 gtest 目标。|

### 4.4 测试（TDD，先红后绿）
| 测试 | 用例 |
|---|---|
| `doris_english_analyzer_test.cpp` | 大小写折叠；数字保留；JSON/标点切分；空串→0 token；连续分隔符不产生空 token；超 64 字节截断（保留首 64，run 不分裂）；纯分隔符串→0 token。**不链接 Arrow/CLucene**。|
| `parallel_tokenizer_test.cpp` | 并行结果（vocab + docs）≡ 串行逐字节结果；线程数 1/4/16 输出一致；docid 与输入行对齐；空 bodies / 含空 body；线程数 > doc 数。**不链接 Arrow/CLucene**。|
| `parquet_corpus_reader_test.cpp` | 用 Arrow C++ 在测试内写小 parquet fixture 再读回：行数=min(N,rows)；Body 内容对齐；缺列抛错；max_docs 截断；N=0 取全部。**链接 Arrow**。|

## 5. 一致性校验

复用并扩展现有 `ALL DOCIDS MATCH`：从真实词表采样高频（stopword 类）/中频/低频 term 与 2–3 词真实短语，对两引擎跑 term + phrase 查询，断言升序 docid 列表逐一相等，失败报告首个 mismatch（term、位置、两侧 docid）。

## 6. 度量

- **导入 = 分词 + 建段**。
- CPU：`getrusage` user+sys，分阶段（tokenize / build）+ 总；
- 峰值内存：`ru_maxrss` − corpus floor = engine-net（沿用现有口径）；
- 索引大小：SNII `.idx` 字节数 / CLucene 段文件求和；
- `--runs` 多轮取中位；
- 输出：SNII vs CLucene 三轴表 + 分词吞吐（docs/s、MB/s）@T 线程。

## 7. 风险

- Arrow 静态链接顺序（parquet → arrow → arrow_bundled_dependencies）可能需调整 —— 中。
- 20M/50M 耗时/占盘 —— 先 5M smoke 验证链路再放大。
- 内存：阶段A 线程局部词表 + 全量 bodies 常驻；50M 约数 GB，受 `--docs` 约束。

## 8. 验收标准

1. `snii_bench --parquet-file <part_000> --docs 5000000 --threads 16 --out-dir <dir>` 跑通；
2. 落盘真实文件可 `ls`、可被对应 reader 重新打开；
3. 一致性 `ALL DOCIDS MATCH`（term + phrase 全过）；
4. 输出三轴 + 分词吞吐表（多轮取中位）；
5. 三个新增 gtest 全绿，且现有 bench/core 测试不回归。
