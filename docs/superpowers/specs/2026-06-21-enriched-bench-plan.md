# 富化基准对比计划（SNII vs CLucene）：算子 × {local,S3} × {单查,并发}

> 日期：2026-06-21 ｜ 来源：brainstorm workflow（20 场景）+ 用户补充（match_all / match_phrase_prefix、local+S3 全覆盖、单查+并发）

## 1. 目标
把当前过窄的 4 查询套件扩成**算子 × harness × 并发**矩阵，全程 `ALL DOCIDS MATCH`（打分另比 topK docid 集 + rank + score 容差）。每个维度都**诚实**：刻意包含 SNII 平手/落败场景，避免 cherry-pick。

## 2. 多维矩阵
- **索引类型**：分词(tokenized, Doris-english) / **不分词(keyword/exact，整值单 term)** / **多索引(单 segment 多 logical index：多分词、或分词+不分词混合写入)**。
- **算子**：exact term / `match`(布尔 AND·OR) / `match_all` / `match_phrase` / `match_phrase_prefix` / prefix·range / keyword exact·range / scoring(BM25 topK，exhaustive·wand·wand_selective)。
- **Harness**：local 成本模型（`MeteredFileReader` 三金指标 + 内存 wall）＋ 真实 S3（OSS 香港 wall，`--oss`）。
- **并发**：单查（延迟 + 三金指标）＋ 并发（N 线程跑查询集 → QPS/吞吐 + 尾延迟 p90/p99 + 峰值 RSS）。对应设计文档第 4 指标「并发查询」。
- **写入侧**：单索引 vs **多索引写入**（写 CPU/峰值内存/体积；正确性：多 logical index 各自可独立打开查询且与单建一致）。

## 3. 场景目录（brainstorm + 补充；P0 优先）
| id | 算子 | 测什么 | 可行性 | 优先 |
|---|---|---|---|---|
| TERM-DF-SWEEP | term×df 桶(very-high→high→mid→low→df1→absent) | 全 df 轴标定三金指标 | existing | P0 |
| AND-HIGH-LOW | 布尔AND(高df+低df) | 驱动=最小df，高频只读覆盖候选窗（核心主张）| new-adapter | P0 |
| OR-MIXED-DF | 布尔OR(df混合) | 反例：无窗剪枝，仍少轮次但字节收窄 | new-adapter | P0 |
| **MATCH-ALL** | match_all(全集) | I/O 基线 / bitmap-all 路径 | new-adapter | P0(补充) |
| PHRASE-LEN-SWEEP | 短语长度{2,3,5,8} rare-led | 轮次随宽度增长(SNII flat vs CLucene N×候选) | existing | P0 |
| PHRASE-STOPWORD-VS-RARE-LED | 短语驱动选择 | df升序驱动→(A)==(B)；CLucene 不对称 | existing | P0 |
| PHRASE-CONJ-GG-RESULT | 合取≫结果 | lazy位置+短路：只解幸存覆盖窗 | existing | P0 |
| MERGE-CROSSSHARD-SCALE | 分片×规模 | 每段轮次优势×段数；找 shard 太小的落败边界 | existing | P0 |
| SCORE-FIDELITY | 打分 score 容差门 | 1字节norm vs SmallFloat → 比 topK集+rank | new-adapter | P0 |
| SCORE-PATH-EQUIV | exhaustive=wand=wand_selective | 内部不变量 + 隔离剪枝收益 | new-adapter | P0 |
| SCORE-TOPK-SELECTIVE | BM25 topK(高idf+低idf) | 窗口max-score剪枝 + 延迟frq取 | new-adapter | P0 |
| AND-DENSE-VS-SPARSE | 稠密vs稀疏AND | 选择性两极；稠密=SNII最差(可能落败) | new-adapter | P1 |
| PHRASE-ALL-STOPWORD | 全停用词短语(无驱动) | SNII 最弱短语，预期字节平/负 | existing | P1 |
| PHRASE-ABSENT-REVERSED | 反序空结果 | 全失败位置校验的轮次有界性 | existing | P1 |
| SCORE-TOPK-RARE-LEAD | 极端idf偏斜topK | 剪枝上界(SNII最强打分) | new-adapter | P1 |
| SCORE-NO-PRUNE-FLOOR | K大/均匀分(无剪枝) | 诚实下界：prelude/窗口开销 SNII 可能略负 | new-adapter | P1 |
| **MATCH-PHRASE-PREFIX** | 末词前缀展开+短语 | 有序term enum + multi-phrase | new-core | P1(补充) |
| PREFIX-ENUM | 前缀/range 有序枚举 | 连续DICT块设计的兑现 | new-core | P1 |
| TERM-SLIM-WINDOW-BOUNDARY | inline↔windowed阈值 | 阈值处字节非单调(诚实) | existing | P2 |
| PHRASE-REPEATED-TERM | 同词两槽 | 正确性角 | existing | P2 |
| PROX-SLOP | slop>0 邻近 | 宽位置窗解码 | new-core | P2 |
| OSS-WALLTIME-TOPK | 打分真实S3 wall | 轮次优势→S3延迟兑现 | new-adapter | P2 |
| **KEYWORD-EXACT** | 不分词整值 exact(低基数 ServiceName/高基数 TraceId) | 不分词索引 exact 查找：低基数=高df单值、高基数=df1；docs-only 无位置 | new-adapter | P0(补充) |
| **KEYWORD-RANGE** | 不分词整值 range/prefix(有序值枚举) | 不分词 range 走有序 term enum + 连续 DICT 块 | new-core | P1(补充) |
| **MULTI-INDEX-WRITE** | 单 segment 多 logical index(多分词 / 分词+不分词混合) | 多索引写入正确性 + 写 CPU/内存/体积；各 index 独立打开查询==单建 | new-adapter | P0(补充) |

## 4. 需新增的代码
1. **corpus df 桶选词**（`corpus_gen`）：公开 `all_dfs`、`term_in_df_bucket(lo,hi)`、`term_at_df(target)`、`df1_term`、`absent_token`、`cooccurring_pair(bandA,bandB,clustered?)`、`extract_phrase(N=8)` + `extract_phrase_shape(df_shape[])` + `reversed_empty_phrase` + `repeated_term_phrase`。
2. **布尔/全集**（两 adapter）：SNII `boolean_and`（df升序驱动，仅取覆盖候选窗，docid-only 交）、`boolean_or`（各term一批dd-block并 union）、`match_all`（全 docid）；CLucene `BooleanQuery`(MUST/SHOULD)、`MatchAllDocsQuery`。
3. **打分**（两 adapter）：SNII `score_query(terms,k,Bm25Params,path)` → `SniiStatsProvider` + `scoring_query_{exhaustive,wand,wand_selective}`；CLucene `IndexSearcher.search(BooleanQuery SHOULD,k)` BM25(k1=1.2,b=0.75)。+ 窗口 considered/fetched/pruned 计数。
4. **harness 矩阵**：main.cpp **scenario-spec 表**（id→{kind,选词规则,k,path}）；`run_query_n`/`query_shards_merged` 支持 bool/scored/match_all/prefix kind；**local 与 S3 同一 spec 驱动**；新增**并发 driver**（线程池跑 spec 集 → QPS+p90/p99+峰值RSS），local 与 S3 各一套。
5. **match_phrase_prefix / prefix-enum**（new-core）：`LogicalIndexReader` 有序 term-enum 迭代器（seek-to-prefix over SampledTermIndex，next 跨连续 DICT 块）→ adapter `prefix_query`/`phrase_prefix_query`。
6. **OSS 打分**：`SniiOssAdapter`/`CluceneOssAdapter` 加 `score_query`。
7. **不分词(keyword)语料 + 索引**：`parquet_corpus_reader` 已能读任意字符串列（ServiceName 低基数/TraceId 高基数/SeverityText）；新增 `keyword_corpus`（每 doc = 整值单 term，identity 分词，建 docs-only 无位置索引）。adapter：SNII 用 `IndexConfig::kDocsOnly` 建不分词 index、exact term 查；CLucene 用 `KeywordAnalyzer` 非分词字段。新增 `keyword_exact_query`（整值 term）。
8. **多索引写入**：SNII adapter `build_multi`（一个 `SniiCompoundWriter` 内 `add_logical_index` 多次：多个分词 index_suffix，或分词+keyword 混合），返回每 index 的体积 + 总写 CPU/峰值内存；reader 各 index 独立 open 查询，断言 == 单建结果。CLucene：一个 IndexWriter 内每 Document 加多个 Field（tokenized + KeywordAnalyzer-per-field via PerFieldAnalyzerWrapper）。

## 5. Rollout（波次，每步 shippable + docid 门）
- **Wave 0（地基）**：(1) df 桶选词 + (4) scenario-spec 表 + 并发 driver 骨架。
- **Wave 1（P0 existing）**：PHRASE-LEN-SWEEP、PHRASE-STOPWORD-VS-RARE-LED、PHRASE-CONJ-GG-RESULT、TERM-DF-SWEEP、MERGE-CROSSSHARD-SCALE。
- **Wave 2（P0 布尔/全集 + 不分词/多索引）**：AND-HIGH-LOW、OR-MIXED-DF、MATCH-ALL；**KEYWORD-EXACT**（不分词 index + exact 查）；**MULTI-INDEX-WRITE**（多 logical index 写入正确性 + 写资源对比）。
- **Wave 3（P0 打分）**：SCORE-FIDELITY（门）→ SCORE-PATH-EQUIV（不变量）→ SCORE-TOPK-SELECTIVE。
- **Wave 4（local+S3+并发 全矩阵）**：把 Wave 1–3 场景跑通 {local,S3}×{单查,并发}，产出对比表。
- **Wave 5（P1 诚实包络）**：AND-DENSE-VS-SPARSE、PHRASE-ALL-STOPWORD、PHRASE-ABSENT-REVERSED、SCORE-TOPK-RARE-LEAD、SCORE-NO-PRUNE-FLOOR。
- **Wave 6（new-core）**：MATCH-PHRASE-PREFIX / PREFIX-ENUM（有序 term enum）+ **KEYWORD-RANGE**（不分词 range/prefix 同走有序枚举）→ PROX-SLOP；OSS-WALLTIME-TOPK。

## 6. 公平性原则
每波刻意配 SNII 平/负场景（OR/dense-AND/all-stopword/no-prune-floor）；打分先立 SCORE-FIDELITY 门与 SCORE-PATH-EQUIV 不变量再做头对头；并发测尾延迟而非仅均值。
