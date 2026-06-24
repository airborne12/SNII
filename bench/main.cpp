// SNII-vs-CLucene S3-access benchmark runner.
//
// On one deterministic synthetic corpus it builds an inverted index with BOTH
// the SNII library and the Doris CLucene library, runs the SAME queries through
// the SAME object-storage cost model (snii::io::MeteredFileReader), and compares
// four metrics: read_at_calls, serial_rounds, range_gets, remote_bytes.
//
// Thesis: SNII front-loads range planning and batches its reads, so it issues
// FEWER serial I/O rounds and range GETs than CLucene's cursor reads, most
// visibly for a multi-term MATCH_PHRASE.
//
// For every query the runner asserts SNII docids == CLucene docids == oracle
// docids (all sorted). Any mismatch exits nonzero.

#include <malloc.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

#include "bench_jsonl.h"
#include "clucene_adapter.h"
#include "corpus_gen.h"
#include "corpus_loader.h"
#include "parallel_tokenizer.h"
#include "parquet_corpus_reader.h"
#include "scenario_gate.h"
#include "snii/io/metered_file_reader.h"
#include "snii_adapter.h"

#ifdef SNII_WITH_S3
#include "clucene_oss_adapter.h"
#include "oss_cleanup.h"
#include "snii/io/s3_object_store.h"
#include "snii_oss_adapter.h"
#endif

namespace {

struct Args {
  uint32_t docs = 2000;
  uint32_t vocab = 2000;
  double zipf = 1.1;
  uint32_t doclen = 12;
  uint64_t seed = 42;
  bool oss = false;  // run the real-OSS wall-clock comparison instead of local
  uint32_t repeat = 1;  // --oss: re-measure each query N times for a distribution
  bool resources = false;     // measure index size / build CPU / peak RSS
  std::string engine = "both";  // --resources scope: snii | clucene | none | both
  uint32_t spill_mib = 0;     // SNII SPIMI spill threshold in MiB (0 = unlimited)
  std::string corpus_file;    // load a REAL corpus (one doc/line) instead of generate
  // E2E real-dataset import mode (activated when parquet_file is non-empty).
  std::string parquet_file;   // source parquet (real OTel-log dataset)
  std::string text_col = "Body";  // string column to index
  uint32_t threads = 0;       // tokenization worker threads (0 = hardware concurrency)
  std::string out_dir;        // persistent index output dir (default under e2e_data)
  bool keep_index = true;     // leave the built index files on disk for inspection
  uint32_t runs = 1;          // build-measurement repeats; report the median
  // Query-only mode (activated when query_dir is non-empty): open the persisted
  // indexes under query_dir and run a query benchmark (no import / build).
  std::string query_dir;      // dir holding snii/index.idx and clucene/
  std::string terms;          // comma-separated query terms (empty = OTel defaults)
  std::string phrase;         // space-separated phrase words (empty = default)
  bool no_merge = false;      // CLucene: flush segments but skip optimize() (no merge)
  uint32_t shard_docs = 0;    // SNII: docs per segment (0 = single segment)
  bool scenarios = false;     // run the enriched scenario catalog (build + compare)
  bool keyword = false;       // run the non-tokenized (keyword) docs-only comparison
  bool multi_index = false;   // run the SNII multi-logical-index write self-check
  bool scoring = false;       // run the SNII BM25 scoring path-equivalence check
  bool prefix = false;        // run prefix / match_phrase_prefix head-to-head
  uint32_t concurrency = 0;   // >0: also run a concurrent throughput pass (N threads)
  double concurrency_seconds = 5.0;  // wall-clock duration of each concurrent pass
  uint32_t concurrency_warmup = 50;  // queries discarded per thread before timing
  std::string bench_out;      // BENCH.4: JSONL output path for the gated scenario suite
};

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (flag == "--docs") {
      a.docs = static_cast<uint32_t>(std::strtoul(next("--docs"), nullptr, 10));
    } else if (flag == "--vocab") {
      a.vocab = static_cast<uint32_t>(std::strtoul(next("--vocab"), nullptr, 10));
    } else if (flag == "--zipf") {
      a.zipf = std::strtod(next("--zipf"), nullptr);
    } else if (flag == "--doclen") {
      a.doclen =
          static_cast<uint32_t>(std::strtoul(next("--doclen"), nullptr, 10));
    } else if (flag == "--seed") {
      a.seed = std::strtoull(next("--seed"), nullptr, 10);
    } else if (flag == "--oss") {
      a.oss = true;
    } else if (flag == "--repeat") {
      a.repeat =
          static_cast<uint32_t>(std::strtoul(next("--repeat"), nullptr, 10));
    } else if (flag == "--resources") {
      a.resources = true;
    } else if (flag == "--engine") {
      a.engine = next("--engine");
    } else if (flag == "--spill-mib") {
      a.spill_mib =
          static_cast<uint32_t>(std::strtoul(next("--spill-mib"), nullptr, 10));
    } else if (flag == "--corpus-file") {
      a.corpus_file = next("--corpus-file");
    } else if (flag == "--parquet-file") {
      a.parquet_file = next("--parquet-file");
    } else if (flag == "--text-col") {
      a.text_col = next("--text-col");
    } else if (flag == "--threads") {
      a.threads =
          static_cast<uint32_t>(std::strtoul(next("--threads"), nullptr, 10));
    } else if (flag == "--out-dir") {
      a.out_dir = next("--out-dir");
    } else if (flag == "--keep-index") {
      a.keep_index = true;
    } else if (flag == "--no-keep-index") {
      a.keep_index = false;
    } else if (flag == "--runs") {
      a.runs = static_cast<uint32_t>(std::strtoul(next("--runs"), nullptr, 10));
      if (a.runs == 0) a.runs = 1;
    } else if (flag == "--query-dir") {
      a.query_dir = next("--query-dir");
    } else if (flag == "--terms") {
      a.terms = next("--terms");
    } else if (flag == "--phrase") {
      a.phrase = next("--phrase");
    } else if (flag == "--no-merge") {
      a.no_merge = true;
    } else if (flag == "--shard-docs") {
      a.shard_docs =
          static_cast<uint32_t>(std::strtoul(next("--shard-docs"), nullptr, 10));
    } else if (flag == "--scenarios") {
      a.scenarios = true;
    } else if (flag == "--keyword") {
      a.keyword = true;
    } else if (flag == "--multi-index") {
      a.multi_index = true;
    } else if (flag == "--scoring") {
      a.scoring = true;
    } else if (flag == "--prefix") {
      a.prefix = true;
    } else if (flag == "--concurrency") {
      a.concurrency =
          static_cast<uint32_t>(std::strtoul(next("--concurrency"), nullptr, 10));
    } else if (flag == "--concurrency-seconds") {
      a.concurrency_seconds = std::strtod(next("--concurrency-seconds"), nullptr);
    } else if (flag == "--concurrency-warmup") {
      a.concurrency_warmup = static_cast<uint32_t>(
          std::strtoul(next("--concurrency-warmup"), nullptr, 10));
    } else if (flag == "--bench-out") {
      a.bench_out = next("--bench-out");
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
      std::exit(2);
    }
  }
  return a;
}

// In-memory oracle: the ground truth for every query, derived directly from the
// corpus token sequences (independent of either index implementation).
class Oracle {
 public:
  explicit Oracle(const bench::Corpus& c) : corpus_(c) {
    for (uint32_t d = 0; d < c.doc_count; ++d) {
      std::set<uint32_t> seen;
      for (uint32_t tid : c.docs[d]) {
        if (seen.insert(tid).second) term_docs_[c.vocab[tid]].insert(d);
      }
    }
  }

  std::vector<uint32_t> term(const std::string& t) const {
    const auto it = term_docs_.find(t);
    if (it == term_docs_.end()) return {};
    return {it->second.begin(), it->second.end()};
  }

  std::vector<uint32_t> phrase(const std::vector<std::string>& words) const {
    std::vector<uint32_t> out;
    if (words.empty()) return out;
    for (uint32_t d = 0; d < corpus_.doc_count; ++d) {
      if (phrase_in_doc(d, words)) out.push_back(d);
    }
    return out;
  }

 private:
  bool phrase_in_doc(uint32_t d, const std::vector<std::string>& words) const {
    const auto& toks = corpus_.docs[d];
    if (toks.size() < words.size()) return false;
    for (size_t i = 0; i + words.size() <= toks.size(); ++i) {
      bool match = true;
      for (size_t k = 0; k < words.size(); ++k) {
        if (corpus_.vocab[toks[i + k]] != words[k]) {
          match = false;
          break;
        }
      }
      if (match) return true;
    }
    return false;
  }

  const bench::Corpus& corpus_;
  std::map<std::string, std::set<uint32_t>> term_docs_;
};

struct QueryResult {
  std::string label;
  bool is_phrase = false;
  size_t hits = 0;
  snii::io::IoMetrics snii;
  snii::io::IoMetrics clucene;
  bool docids_match = false;
};

std::string join_words(const std::vector<std::string>& w) {
  std::string s;
  for (size_t i = 0; i < w.size(); ++i) {
    if (i) s.push_back(' ');
    s += w[i];
  }
  return s;
}

double ratio(uint64_t clucene, uint64_t snii_val) {
  if (snii_val == 0) return clucene == 0 ? 1.0 : static_cast<double>(clucene);
  return static_cast<double>(clucene) / static_cast<double>(snii_val);
}

// Best-effort git revision of the bench tree, captured into each JSONL row so a
// gated measurement can be replayed at the exact source it was produced from.
// Returns "unknown" if git is unavailable (never fails the run).
std::string git_rev() {
  std::string rev;
  if (FILE* p = popen("git rev-parse --short HEAD 2>/dev/null", "r")) {
    char buf[64] = {0};
    if (std::fgets(buf, sizeof(buf), p) != nullptr) rev = buf;
    pclose(p);
  }
  while (!rev.empty() && (rev.back() == '\n' || rev.back() == '\r')) rev.pop_back();
  return rev.empty() ? std::string("unknown") : rev;
}

void print_metrics_row(const char* engine, const snii::io::IoMetrics& m) {
  // remote_bytes = 1MiB-block-aligned bytes (FileCache occupancy);
  // request_bytes = exact bytes the query asked for (matches the design's
  // "all ranges' returned bytes" on a direct/no-cache S3 read).
  std::printf("  %-8s read_at=%-6llu serial_rounds=%-4llu range_gets=%-4llu "
              "remote_bytes=%-9llu request_bytes=%-9llu\n",
              engine,
              static_cast<unsigned long long>(m.read_at_calls),
              static_cast<unsigned long long>(m.serial_rounds),
              static_cast<unsigned long long>(m.range_gets),
              static_cast<unsigned long long>(m.remote_bytes),
              static_cast<unsigned long long>(m.total_request_bytes));
}

#ifdef SNII_WITH_S3
// One real-OSS query result: wall-clock latency for both engines plus their
// (identical cost model) I/O metrics and the docid-equality verdict.
struct OssQueryResult {
  std::string label;
  size_t hits = 0;
  double snii_wall_ms = 0.0;
  double clucene_wall_ms = 0.0;
  snii::io::IoMetrics snii;
  snii::io::IoMetrics clucene;
  bool docids_match = false;
};

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// Builds the OSS connection config. Credentials are read from the environment
// (SNII_OSS_AK / SNII_OSS_SK) and never hardcoded. A per-run prefix suffix keeps
// concurrent runs from colliding on the same object keys.
bool build_oss_config(snii::io::S3Config* cfg) {
  const char* ak = std::getenv("SNII_OSS_AK");
  const char* sk = std::getenv("SNII_OSS_SK");
  if (ak == nullptr || sk == nullptr || ak[0] == '\0' || sk[0] == '\0') {
    std::fprintf(stderr,
                 "FATAL: --oss requires SNII_OSS_AK and SNII_OSS_SK in the "
                 "environment\n");
    return false;
  }
  cfg->endpoint = "oss-cn-hongkong.aliyuncs.com";
  cfg->region = "cn-hongkong";
  cfg->bucket = "doris-community-test";
  cfg->prefix = "cloud_regression/snii_oss_bench/run_" +
                std::to_string(static_cast<long long>(::getpid()));
  cfg->ak = ak;
  cfg->sk = sk;
  return true;
}

void print_oss_metrics_row(const char* engine, double wall_ms,
                           const snii::io::IoMetrics& m) {
  // request_bytes = exact bytes fetched from OSS (what S3FileReader actually
  // transfers); remote_bytes = 1MiB-block-aligned cost-model count.
  std::printf("  %-8s wall_ms=%-9.1f serial_rounds=%-4llu range_gets=%-4llu "
              "remote_bytes=%-9llu request_bytes=%-9llu\n",
              engine, wall_ms,
              static_cast<unsigned long long>(m.serial_rounds),
              static_cast<unsigned long long>(m.range_gets),
              static_cast<unsigned long long>(m.remote_bytes),
              static_cast<unsigned long long>(m.total_request_bytes));
}

// Prints min / median / p90 / mean of a wall-clock sample set (ms). Sorts a
// local copy so callers keep their order. Empty input prints nothing.
void print_wall_distribution(const char* engine, std::vector<double> samples) {
  if (samples.empty()) return;
  std::sort(samples.begin(), samples.end());
  const size_t n = samples.size();
  const double med = samples[n / 2];
  const double p90 = samples[static_cast<size_t>(0.9 * (n - 1) + 0.5)];
  double sum = 0.0;
  for (double v : samples) sum += v;
  std::printf("  %-8s n=%-3zu min=%-7.1f median=%-7.1f p90=%-7.1f mean=%-7.1f\n",
              engine, n, samples.front(), med, p90, sum / static_cast<double>(n));
}

// Runs the full real-OSS comparison. Returns the process exit code (0 on success
// with all docids matching, nonzero otherwise).
// Concurrent S3 throughput matrix (defined after run_concurrent below). Each worker
// opens its OWN S3 reader chain over the already-uploaded index via open_uploaded.
void run_oss_concurrent(const Args& args, const bench::Corpus& corpus,
                        const snii::io::S3Config& cfg, const std::string& snii_key,
                        const std::vector<std::string>& clu_names);

int run_oss_mode(const Args& args, const bench::Corpus& corpus,
                 const Oracle& oracle) {
  snii::io::S3Config cfg;
  if (!build_oss_config(&cfg)) return 2;

  // One process-wide aws InitAPI/ShutdownAPI guard on the main stack.
  snii::io::AwsApiGuard aws_guard;

  std::printf("\n=== REAL-OSS mode ===\n");
  std::printf("oss endpoint=%s bucket=%s prefix=%s\n", cfg.endpoint.c_str(),
              cfg.bucket.c_str(), cfg.prefix.c_str());

  bench::SniiOssAdapter snii_idx;
  bench::CluceneOssAdapter cl_idx;
  std::vector<std::string> all_keys;  // for best-effort cleanup at the end
  try {
    std::printf("building + uploading SNII index to OSS...\n");
    auto t0 = std::chrono::steady_clock::now();
    snii_idx.build_upload_and_open(corpus, cfg);
    std::printf("  SNII uploaded in %.0f ms (key=%s)\n", ms_since(t0),
                snii_idx.uploaded_key().c_str());
    {
      const uint64_t bsbf_bytes = snii_idx.bsbf_section_bytes();
      const char* tenv = std::getenv("SNII_BSBF_RESIDENT_MAX");
      const uint64_t thr = (tenv != nullptr) ? std::strtoull(tenv, nullptr, 10)
                                             : 256ull * 1024;
      std::printf("  SNII bsbf filter = %llu bytes -> tier %s (threshold=%llu)\n",
                  static_cast<unsigned long long>(bsbf_bytes),
                  bsbf_bytes != 0 && bsbf_bytes <= thr ? "L0(resident)" : "L1(on-demand)",
                  static_cast<unsigned long long>(thr));
    }
    all_keys.push_back(snii_idx.uploaded_key());

    std::printf("building + uploading CLucene index to OSS...\n");
    t0 = std::chrono::steady_clock::now();
    cl_idx.build_upload_and_open(corpus, cfg);
    std::printf("  CLucene uploaded %zu files in %.0f ms\n",
                cl_idx.uploaded_keys().size(), ms_since(t0));
    for (const std::string& k : cl_idx.uploaded_keys()) all_keys.push_back(k);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: OSS index build/upload failed: %s\n", e.what());
    bench::oss_delete_objects(cfg, all_keys);
    return 1;
  }

  const std::string high_term = corpus.vocab[bench::highest_df_term(corpus)];
  const std::string mid_term = corpus.vocab[bench::mid_df_term(corpus)];
  const std::string low_term = corpus.vocab[bench::low_df_term(corpus)];
  const std::vector<std::string> phrase = bench::extract_phrase(corpus, 5);

  std::printf("query terms: high-df='%s'(df=%zu) mid-df='%s'(df=%zu) "
              "low-df='%s'(df=%zu)\n",
              high_term.c_str(), oracle.term(high_term).size(), mid_term.c_str(),
              oracle.term(mid_term).size(), low_term.c_str(),
              oracle.term(low_term).size());
  std::printf("phrase: '%s'\n", join_words(phrase).c_str());

  std::vector<OssQueryResult> results;
  bool all_match = true;

  auto run_term = [&](const std::string& label, const std::string& term) {
    OssQueryResult r;
    r.label = label + " '" + term + "'";
    std::vector<uint32_t> sdoc, cdoc;
    try {
      auto t0 = std::chrono::steady_clock::now();
      snii_idx.term_query(term, &sdoc, &r.snii);
      r.snii_wall_ms = ms_since(t0);
      t0 = std::chrono::steady_clock::now();
      cl_idx.term_query(term, &cdoc, &r.clucene);
      r.clucene_wall_ms = ms_since(t0);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: OSS term query '%s' failed: %s\n",
                   term.c_str(), e.what());
      bench::oss_delete_objects(cfg, all_keys);
      std::exit(1);
    }
    const std::vector<uint32_t> odoc = oracle.term(term);
    r.hits = sdoc.size();
    r.docids_match = (sdoc == cdoc) && (sdoc == odoc);
    if (!r.docids_match) {
      all_match = false;
      std::fprintf(stderr,
                   "MISMATCH on %s: snii=%zu clucene=%zu oracle=%zu\n",
                   r.label.c_str(), sdoc.size(), cdoc.size(), odoc.size());
    }
    results.push_back(r);
  };

  auto run_phrase = [&](const std::vector<std::string>& words) {
    OssQueryResult r;
    r.label = "PHRASE '" + join_words(words) + "'";
    std::vector<uint32_t> sdoc, cdoc;
    try {
      auto t0 = std::chrono::steady_clock::now();
      snii_idx.phrase_query(words, &sdoc, &r.snii);
      r.snii_wall_ms = ms_since(t0);
      t0 = std::chrono::steady_clock::now();
      cl_idx.phrase_query(words, &cdoc, &r.clucene);
      r.clucene_wall_ms = ms_since(t0);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: OSS phrase query failed: %s\n", e.what());
      bench::oss_delete_objects(cfg, all_keys);
      std::exit(1);
    }
    const std::vector<uint32_t> odoc = oracle.phrase(words);
    r.hits = sdoc.size();
    r.docids_match = (sdoc == cdoc) && (sdoc == odoc);
    if (!r.docids_match) {
      all_match = false;
      std::fprintf(stderr,
                   "MISMATCH on PHRASE: snii=%zu clucene=%zu oracle=%zu\n",
                   sdoc.size(), cdoc.size(), odoc.size());
    }
    results.push_back(r);
  };

  run_term("TERM high-df", high_term);
  run_term("TERM mid-df", mid_term);
  run_term("TERM low-df", low_term);
  if (!phrase.empty()) {
    run_phrase(phrase);
  } else {
    std::fprintf(stderr, "WARNING: no 5-token phrase found in corpus\n");
  }

  // --- Boolean / match-all / prefix / match_phrase_prefix on the tokenized index. ---
  auto run_op2 = [&](const std::string& label, auto snii_fn, auto clu_fn) {
    OssQueryResult r;
    r.label = label;
    std::vector<uint32_t> sdoc, cdoc;
    try {
      auto t0 = std::chrono::steady_clock::now();
      snii_fn(&sdoc, &r.snii);
      r.snii_wall_ms = ms_since(t0);
      t0 = std::chrono::steady_clock::now();
      clu_fn(&cdoc, &r.clucene);
      r.clucene_wall_ms = ms_since(t0);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "WARNING: OSS op '%s' failed: %s\n", label.c_str(),
                   e.what());
      return;
    }
    r.hits = sdoc.size();
    r.docids_match = (sdoc == cdoc);
    if (!r.docids_match) {
      all_match = false;
      std::fprintf(stderr, "MISMATCH on %s: snii=%zu clucene=%zu\n", label.c_str(),
                   sdoc.size(), cdoc.size());
    }
    results.push_back(r);
  };

  const std::vector<std::string> and_terms = {high_term, low_term};
  run_op2("AND high+low",
          [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
            snii_idx.boolean_and(and_terms, d, m);
          },
          [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
            cl_idx.boolean_and(and_terms, d, m);
          });
  const std::vector<std::string> or_terms = {high_term, mid_term, low_term};
  run_op2("OR high+mid+low",
          [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
            snii_idx.boolean_or(or_terms, d, m);
          },
          [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
            cl_idx.boolean_or(or_terms, d, m);
          });
  run_op2("MATCH-ALL",
          [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
            snii_idx.match_all(d, m);
          },
          [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
            cl_idx.match_all(d, m);
          });

  // Bounded prefix (<=200 expansions) from a real word, via SNII's enumeration.
  auto bounded_prefix = [&](const std::string& w) -> std::string {
    for (size_t L = 1; L <= w.size(); ++L) {
      std::string p = w.substr(0, L);
      if (snii_idx.enumerate_prefix(p).size() <= 200) return p;
    }
    return w;
  };
  const std::string pfx = bounded_prefix(high_term);
  if (!pfx.empty()) {
    run_op2("PREFIX '" + pfx + "*'",
            [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
              snii_idx.prefix_query(pfx, d, m);
            },
            [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
              cl_idx.prefix_query(pfx, d, m);
            });
  }
  if (phrase.size() >= 2) {
    const std::vector<std::string> fixed(phrase.begin(), phrase.end() - 1);
    const std::string pfx2 = bounded_prefix(phrase.back());
    const std::vector<std::string> exp = snii_idx.enumerate_prefix(pfx2);
    run_op2("MATCH_PHRASE_PREFIX (" + std::to_string(exp.size()) + " exp)",
            [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
              snii_idx.phrase_prefix_query(fixed, exp, d, m);
            },
            [&](std::vector<uint32_t>* d, snii::io::IoMetrics* m) {
              cl_idx.phrase_prefix_query(fixed, exp, d, m);
            });
  }

  // --- Keyword (non-tokenized, docs-only): a SECOND OSS index pair. ---
  bench::SniiOssAdapter snii_kw;
  bench::CluceneOssAdapter cl_kw;
  snii_kw.set_docs_only(true);
  cl_kw.set_docs_only(true);
  {
    static const char* kSvc[] = {"frontend",     "cartservice", "checkoutservice",
                                 "paymentservice", "shippingservice",
                                 "emailservice", "currencyservice"};
    std::vector<std::string> values(corpus.doc_count);
    for (uint32_t i = 0; i < corpus.doc_count; ++i) values[i] = kSvc[i % 7];
    for (uint32_t i = 0; i < corpus.doc_count / 1000; ++i)
      values[i] = "trace" + std::to_string(i);
    bench::Corpus kw = bench::keyword_corpus(values);
    try {
      snii_kw.build_upload_and_open(kw, cfg);
      all_keys.push_back(snii_kw.uploaded_key());
      cl_kw.build_upload_and_open(kw, cfg);
      for (const std::string& k : cl_kw.uploaded_keys()) all_keys.push_back(k);
      const std::string kv = kw.vocab[bench::term_in_df_bucket(kw, 0.1, 1.0)];
      OssQueryResult r;
      r.label = "KEYWORD exact '" + kv + "'";
      std::vector<uint32_t> sdoc, cdoc;
      auto t0 = std::chrono::steady_clock::now();
      snii_kw.term_query(kv, &sdoc, &r.snii);
      r.snii_wall_ms = ms_since(t0);
      t0 = std::chrono::steady_clock::now();
      cl_kw.term_query(kv, &cdoc, &r.clucene);
      r.clucene_wall_ms = ms_since(t0);
      r.hits = sdoc.size();
      r.docids_match = (sdoc == cdoc);
      if (!r.docids_match) all_match = false;
      results.push_back(r);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "WARNING: OSS keyword build failed: %s\n", e.what());
    }
  }

  std::printf("\n=== REAL-OSS wall-clock comparison (CLucene vs SNII, "
              "same cost model, real OSS GETs) ===\n");
  for (const OssQueryResult& r : results) {
    std::printf("[%s]  hits=%zu  docids_match=%s\n", r.label.c_str(), r.hits,
                r.docids_match ? "YES" : "NO");
    print_oss_metrics_row("CLucene", r.clucene_wall_ms, r.clucene);
    print_oss_metrics_row("SNII", r.snii_wall_ms, r.snii);
    std::printf("  ratio    wall_ms(CL/SNII)=%.2f  serial_rounds(CL/SNII)=%.2f  "
                "range_gets(CL/SNII)=%.2f\n",
                ratio(static_cast<uint64_t>(r.clucene_wall_ms),
                      static_cast<uint64_t>(r.snii_wall_ms)),
                ratio(r.clucene.serial_rounds, r.snii.serial_rounds),
                ratio(r.clucene.range_gets, r.snii.range_gets));
  }

  // Optional distribution pass: re-measure each query args.repeat times. Every
  // call resets the cost model and re-issues real OSS GETs (cold), so the spread
  // exposes per-GET OSS latency jitter -- which dominates tiny single-round
  // queries and explains why a single mid-df sample can land either way.
  if (args.repeat > 1) {
    std::printf("\n=== wall-clock distribution over %u cold repeats "
                "(real-OSS jitter) ===\n",
                args.repeat);
    struct Q {
      const char* label;
      bool is_phrase;
      std::string term;
    };
    std::vector<Q> queries = {{"TERM high-df", false, high_term},
                              {"TERM mid-df", false, mid_term},
                              {"TERM low-df", false, low_term}};
    if (!phrase.empty()) queries.push_back({"PHRASE", true, std::string()});
    for (const Q& q : queries) {
      std::vector<double> snii_ms, cl_ms;
      std::vector<uint32_t> sdoc, cdoc;
      snii::io::IoMetrics sm, cm;
      try {
        for (uint32_t i = 0; i < args.repeat; ++i) {
          auto t0 = std::chrono::steady_clock::now();
          if (q.is_phrase) {
            snii_idx.phrase_query(phrase, &sdoc, &sm);
          } else {
            snii_idx.term_query(q.term, &sdoc, &sm);
          }
          snii_ms.push_back(ms_since(t0));
          t0 = std::chrono::steady_clock::now();
          if (q.is_phrase) {
            cl_idx.phrase_query(phrase, &cdoc, &cm);
          } else {
            cl_idx.term_query(q.term, &cdoc, &cm);
          }
          cl_ms.push_back(ms_since(t0));
        }
      } catch (const std::exception& e) {
        std::fprintf(stderr, "WARNING: repeat measurement failed: %s\n",
                     e.what());
      }
      std::printf("[%s] (hits=%zu)\n", q.label, sdoc.size());
      print_wall_distribution("CLucene", cl_ms);
      print_wall_distribution("SNII", snii_ms);
    }
  }

  // Concurrent S3 throughput matrix (single vs N threads), term/phrase scenarios.
  // The aws_guard above stays live across the worker pool (required for S3 init).
  if (args.concurrency != 0) {
    try {
      run_oss_concurrent(args, corpus, cfg, snii_idx.object_key(),
                         cl_idx.file_names());
    } catch (const std::exception& e) {
      std::fprintf(stderr, "WARN: OSS concurrent pass failed: %s\n", e.what());
    }
  }

  // Best-effort cleanup of the uploaded OSS objects.
  const size_t removed = bench::oss_delete_objects(cfg, all_keys);
  std::printf("\ncleanup: removed %zu / %zu OSS objects\n", removed,
              all_keys.size());

  std::printf("\nresult: %s\n",
              all_match ? "ALL DOCIDS MATCH" : "DOCID MISMATCH");
  return all_match ? 0 : 1;
}
#endif  // SNII_WITH_S3

// Cumulative process CPU (user+sys) seconds via getrusage(RUSAGE_SELF).
double cpu_seconds() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  const auto tv = [](const timeval& t) {
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) / 1e6;
  };
  return tv(ru.ru_utime) + tv(ru.ru_stime);
}

// Process peak resident set size (high-water mark) in MiB. ru_maxrss is KiB on Linux.
double peak_rss_mib() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_maxrss) / 1024.0;
}

// Current (not peak) resident set size in MiB, read from /proc/self/statm
// (field 2 = resident pages). Used to print the corpus floor before the build.
double current_rss_mib() {
  long rss_pages = 0;
  if (FILE* f = std::fopen("/proc/self/statm", "r")) {
    long total = 0;
    if (std::fscanf(f, "%ld %ld", &total, &rss_pages) != 2) rss_pages = 0;
    std::fclose(f);
  }
  const long page = sysconf(_SC_PAGESIZE);
  return static_cast<double>(rss_pages) * static_cast<double>(page) / (1024.0 * 1024.0);
}

double mib(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

// Applies the same MiB flush/spill threshold to either engine so build memory
// is bounded (or unbounded) IDENTICALLY for a fair comparison: SNII spills its
// SPIMI buffer at `spill_mib`; CLucene flushes a segment at the same RAM size.
void apply_spill(bench::SniiAdapter& idx, uint32_t spill_mib) {
  idx.set_spill_threshold_bytes(static_cast<size_t>(spill_mib) * 1024u * 1024u);
}
void apply_spill(bench::CluceneAdapter& idx, uint32_t spill_mib) {
  idx.set_ram_buffer_mb(static_cast<double>(spill_mib));  // 0 = no auto flush
}
template <typename Adapter>
void apply_spill(Adapter&, uint32_t) {}

// CLucene-only: flush segments without optimize() (no merge). No-op for SNII.
void apply_no_merge(bench::CluceneAdapter& idx, bool v) { idx.set_no_merge(v); }
template <typename Adapter>
void apply_no_merge(Adapter&, bool) {}

// Builds one engine over the corpus, reporting its on-disk index size and the
// CPU it took. Returns nothing; peak RSS is read at process exit by the caller.
template <typename Adapter>
void build_and_report(const char* name, const bench::Corpus& corpus,
                      uint32_t spill_mib, bool no_merge) {
  Adapter idx;
  apply_spill(idx, spill_mib);
  apply_no_merge(idx, no_merge);
  std::printf("  %-8s pre_build_rss=%.1f MiB (corpus floor)\n", name, current_rss_mib());
  const double c0 = cpu_seconds();
  const auto w0 = std::chrono::steady_clock::now();
  idx.build_and_open(corpus);
  const double build_cpu = cpu_seconds() - c0;
  const double build_wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
  std::printf("  %-8s index_bytes=%-12llu (%7.2f MiB)  build_cpu_s=%-7.2f "
              "build_wall_s=%-7.2f\n",
              name, static_cast<unsigned long long>(idx.index_bytes()),
              mib(idx.index_bytes()), build_cpu, build_wall);
}

// --resources mode: index size, build CPU, and peak RSS for the selected engine.
// Run with --engine snii|clucene|none in SEPARATE processes to isolate peak RSS
// (each process holds the SAME corpus, so engine RSS == peak - none-baseline).
int run_resources_mode(const Args& args, const bench::Corpus& corpus) {
  std::printf("=== resource comparison (engine=%s) ===\n", args.engine.c_str());
  std::printf("  corpus held in memory; peak RSS includes it (use --engine none "
              "for the baseline)\n");
  if (args.spill_mib != 0) {
    std::printf("  SNII spill threshold: %u MiB (out-of-core spill + k-way "
                "merge)\n",
                args.spill_mib);
  }
  const bool snii = args.engine == "snii" || args.engine == "both";
  const bool clu = args.engine == "clucene" || args.engine == "both";
  try {
    if (snii)
      build_and_report<bench::SniiAdapter>("SNII", corpus, args.spill_mib,
                                           args.no_merge);
    // Same threshold for CLucene: its RAM-buffer flush matches SNII's spill.
    if (clu)
      build_and_report<bench::CluceneAdapter>("CLucene", corpus, args.spill_mib,
                                              args.no_merge);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: resource build failed: %s\n", e.what());
    return 1;
  }
  std::printf("  peak_rss=%.1f MiB (process high-water; engine=%s)\n",
              peak_rss_mib(), args.engine.c_str());
  return 0;
}

// Median of a sample set (sorts a local copy). Empty input returns 0.
double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// Lists the on-disk index files with sizes (the "real index files" deliverable).
void print_index_files(const char* name, const std::vector<std::string>& files) {
  uint64_t total = 0;
  std::printf("  %s index files:\n", name);
  for (const std::string& f : files) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(f, ec);
    const uint64_t b = ec ? 0 : static_cast<uint64_t>(sz);
    total += b;
    std::printf("    %-52s %10llu B (%7.2f MiB)\n", f.c_str(),
                static_cast<unsigned long long>(b), mib(b));
  }
  std::printf("    %-52s %10llu B (%7.2f MiB)\n", "TOTAL",
              static_cast<unsigned long long>(total), mib(total));
}

// E2E real-dataset import mode: read a parquet text column, tokenize it with the
// shared Doris-english analyzer across `threads` workers, build a persistent
// on-disk index with each engine, verify cross-engine query consistency (and
// that the persisted SNII container reopens), and report import CPU / peak RSS /
// index size. Returns the process exit code (0 = success + all docids match).
int run_e2e_mode(const Args& args) {
  const bool want_snii = args.engine == "snii" || args.engine == "both";
  const bool want_clu = args.engine == "clucene" || args.engine == "both";
  const uint32_t threads =
      args.threads != 0 ? args.threads
                        : std::max(1u, std::thread::hardware_concurrency());
  std::string out_dir = args.out_dir.empty()
                            ? std::string("/mnt/disk15/jiangkai/e2e_data/run")
                            : args.out_dir;
  const std::string snii_path = out_dir + "/snii/index.idx";
  const std::string clu_dir = out_dir + "/clucene";

  std::printf("\n=== E2E real-dataset import "
              "(parquet -> Doris-english tokenize -> build -> verify) ===\n");
  std::printf("parquet=%s column=%s max_docs=%u threads=%u engine=%s\n",
              args.parquet_file.c_str(), args.text_col.c_str(), args.docs,
              threads, args.engine.c_str());
  std::printf("out_dir=%s keep_index=%s runs=%u spill_mib=%u\n", out_dir.c_str(),
              args.keep_index ? "yes" : "no", args.runs, args.spill_mib);

  // 1. Read the text column from the real parquet dataset.
  std::vector<std::string> bodies;
  uint64_t raw_bytes = 0;
  try {
    const auto w0 = std::chrono::steady_clock::now();
    bodies = bench::read_text_column(args.parquet_file, args.text_col, args.docs);
    const double read_wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    for (const std::string& b : bodies) raw_bytes += b.size();
    std::printf("  read %zu rows (%.1f MiB raw text) in %.2f s\n", bodies.size(),
                mib(raw_bytes), read_wall);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: parquet read failed: %s\n", e.what());
    return 1;
  }
  if (bodies.empty()) {
    std::fprintf(stderr, "FATAL: parquet column yielded 0 rows\n");
    return 1;
  }

  // 2. Parallel Doris-english tokenization (the only multi-threaded stage).
  const double tc0 = cpu_seconds();
  const auto tw0 = std::chrono::steady_clock::now();
  bench::Corpus corpus = bench::tokenize_corpus(bodies, threads);
  const double tok_cpu = cpu_seconds() - tc0;
  const double tok_wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - tw0).count();
  uint64_t total_tokens = 0;
  for (const auto& d : corpus.docs) total_tokens += d.size();
  const double corpus_floor = current_rss_mib();
  std::printf("  tokenized: docs=%u vocab=%zu tokens=%llu  "
              "tok_cpu_s=%.2f tok_wall_s=%.2f\n",
              corpus.doc_count, corpus.vocab.size(),
              static_cast<unsigned long long>(total_tokens), tok_cpu, tok_wall);
  std::printf("  tokenize throughput: %.0f docs/s, %.1f MiB/s (raw) @ %u threads\n",
              tok_wall > 0 ? bodies.size() / tok_wall : 0.0,
              tok_wall > 0 ? mib(raw_bytes) / tok_wall : 0.0, threads);
  std::printf("  corpus floor RSS=%.1f MiB (tokens held in memory)\n", corpus_floor);
  // Raw bodies are no longer needed (tokens are the source of truth); free them
  // so the build's peak RSS is not inflated by the raw text.
  { std::vector<std::string>().swap(bodies); }

  // Real-OSS latency mode: route the real-parquet corpus into the OSS comparison
  // (build + upload both indexes to OSS, then query over REAL ranged GETs and
  // measure wall-clock). The oracle is built only at safe scales.
  if (args.oss) {
#ifdef SNII_WITH_S3
    if (corpus.doc_count > 50'000'000) {
      std::fprintf(stderr, "FATAL: --oss caps at 50M docs (oracle + upload); "
                           "use a smaller --docs\n");
      return 2;
    }
    const Oracle oracle(corpus);
    return run_oss_mode(args, corpus, oracle);
#else
    std::fprintf(stderr, "FATAL: --oss requires building with -DSNII_WITH_S3=ON\n");
    return 2;
#endif
  }

  // 3. Build the selected engine(s), persisting to out_dir. The final iteration's
  //    index is the one kept on disk; --runs repeats the build to take a median.
  bench::SniiAdapter snii_idx;
  bench::CluceneAdapter cl_idx;
  apply_spill(snii_idx, args.spill_mib);
  apply_spill(cl_idx, args.spill_mib);
  std::vector<double> snii_cpu, snii_wall, clu_cpu, clu_wall;
  uint64_t snii_bytes = 0, clu_bytes = 0;
  try {
    for (uint32_t r = 0; r < args.runs; ++r) {
      const bool last = (r + 1 == args.runs);
      const bool keep = last && args.keep_index;
      if (want_snii) {
        // A throwaway adapter for the non-final measurement runs; the final build
        // uses the persistent snii_idx kept alive for the consistency check.
        const double c0 = cpu_seconds();
        const auto w0 = std::chrono::steady_clock::now();
        if (last) {
          snii_idx.build_at(snii_path, corpus, keep);
        } else {
          bench::SniiAdapter tmp;
          apply_spill(tmp, args.spill_mib);
          tmp.build_at(snii_path, corpus, /*keep_on_disk=*/false);
        }
        snii_cpu.push_back(cpu_seconds() - c0);
        snii_wall.push_back(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count());
        if (last) snii_bytes = snii_idx.index_bytes();
      }
      if (want_clu) {
        const double c0 = cpu_seconds();
        const auto w0 = std::chrono::steady_clock::now();
        if (last) {
          cl_idx.build_at(clu_dir, corpus, keep);
        } else {
          bench::CluceneAdapter tmp;
          apply_spill(tmp, args.spill_mib);
          tmp.build_at(clu_dir, corpus, /*keep_on_disk=*/false);
        }
        clu_cpu.push_back(cpu_seconds() - c0);
        clu_wall.push_back(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count());
        if (last) clu_bytes = cl_idx.index_bytes();
      }
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: E2E build failed: %s\n", e.what());
    return 1;
  }

  // 4. Cross-engine consistency: term (high/mid/low df) + a real phrase. Each is
  //    asserted equal across SNII, CLucene, and the in-memory oracle.
  bool all_match = true;
  if (want_snii && want_clu) {
    // The in-memory Oracle (map<string,set<docid>>) is a third cross-check, but
    // it is itself a full inverted index in the most memory-expensive layout, so
    // at very large doc counts it would dwarf both engines and risk OOM. Above
    // the threshold we drop it and rely on the direct SNII-vs-CLucene equality
    // (the primary consistency guarantee); below it we keep all three.
    constexpr uint32_t kOracleMaxDocs = 50'000'000;
    const bool use_oracle = corpus.doc_count <= kOracleMaxDocs;
    std::unique_ptr<Oracle> oracle;
    if (use_oracle) {
      oracle = std::make_unique<Oracle>(corpus);
    } else {
      std::printf("  (corpus > %u docs: skipping in-memory oracle to bound RAM; "
                  "verifying SNII == CLucene directly)\n",
                  kOracleMaxDocs);
    }
    const std::string high_term = corpus.vocab[bench::highest_df_term(corpus)];
    const std::string mid_term = corpus.vocab[bench::mid_df_term(corpus)];
    const std::string low_term = corpus.vocab[bench::low_df_term(corpus)];
    const std::vector<std::string> phrase = bench::extract_phrase(corpus, 3);
    std::printf("query terms: high-df='%s' mid-df='%s' low-df='%s'  phrase='%s'\n",
                high_term.c_str(), mid_term.c_str(), low_term.c_str(),
                join_words(phrase).c_str());

    auto check_term = [&](const std::string& term) {
      std::vector<uint32_t> sdoc, cdoc;
      snii::io::IoMetrics sm, cm;
      snii_idx.term_query(term, &sdoc, &sm);
      cl_idx.term_query(term, &cdoc, &cm);
      const bool engines_eq = (sdoc == cdoc);
      const bool oracle_eq = !use_oracle || sdoc == oracle->term(term);
      if (!(engines_eq && oracle_eq)) {
        all_match = false;
        std::fprintf(stderr, "MISMATCH term '%s': snii=%zu clucene=%zu%s\n",
                     term.c_str(), sdoc.size(), cdoc.size(),
                     use_oracle ? (" oracle=" + std::to_string(
                                       oracle->term(term).size())).c_str()
                                : "");
      }
    };
    check_term(high_term);
    check_term(mid_term);
    check_term(low_term);
    if (!phrase.empty()) {
      std::vector<uint32_t> sdoc, cdoc;
      snii::io::IoMetrics sm, cm;
      snii_idx.phrase_query(phrase, &sdoc, &sm);
      cl_idx.phrase_query(phrase, &cdoc, &cm);
      const bool engines_eq = (sdoc == cdoc);
      const bool oracle_eq = !use_oracle || sdoc == oracle->phrase(phrase);
      if (!(engines_eq && oracle_eq)) {
        all_match = false;
        std::fprintf(stderr, "MISMATCH phrase: snii=%zu clucene=%zu\n",
                     sdoc.size(), cdoc.size());
      }
    }

    // 4b. Reopen the persisted SNII container with a FRESH reader and confirm the
    //     same docids -- proving the on-disk file is a real, reopenable index.
    if (args.keep_index) {
      try {
        bench::SniiAdapter reopened;
        reopened.open_existing(snii_path);
        std::vector<uint32_t> rdoc, sdoc;
        snii::io::IoMetrics m;
        reopened.term_query(high_term, &rdoc, &m);
        snii_idx.term_query(high_term, &sdoc, &m);
        if (rdoc != sdoc) {
          all_match = false;
          std::fprintf(stderr, "MISMATCH reopen: fresh=%zu original=%zu\n",
                       rdoc.size(), sdoc.size());
        } else {
          std::printf("  reopen check: persisted SNII .idx reopened, "
                      "term '%s' -> %zu docs (match)\n",
                      high_term.c_str(), rdoc.size());
        }
      } catch (const std::exception& e) {
        all_match = false;
        std::fprintf(stderr, "FATAL: reopen of %s failed: %s\n",
                     snii_path.c_str(), e.what());
      }
    }
  }

  // 5. Report: import CPU / peak RSS / index size.
  std::printf("\n=== E2E import metrics (median of %u run%s) ===\n", args.runs,
              args.runs == 1 ? "" : "s");
  std::printf("  tokenize (shared): cpu_s=%.2f wall_s=%.2f threads=%u\n", tok_cpu,
              tok_wall, threads);
  if (want_snii) {
    std::printf("  SNII     index_bytes=%-12llu (%7.2f MiB)  build_cpu_s=%.2f  "
                "build_wall_s=%.2f\n",
                static_cast<unsigned long long>(snii_bytes), mib(snii_bytes),
                median(snii_cpu), median(snii_wall));
  }
  if (want_clu) {
    std::printf("  CLucene  index_bytes=%-12llu (%7.2f MiB)  build_cpu_s=%.2f  "
                "build_wall_s=%.2f\n",
                static_cast<unsigned long long>(clu_bytes), mib(clu_bytes),
                median(clu_cpu), median(clu_wall));
  }
  const double peak = peak_rss_mib();
  std::printf("  peak_rss=%.1f MiB (process high-water; corpus floor=%.1f MiB; "
              "engine-net=%.1f MiB)\n",
              peak, corpus_floor, peak - corpus_floor);
  if (args.engine == "both") {
    std::printf("  NOTE: peak RSS covers BOTH builds; for clean per-engine memory "
                "run --engine snii / --engine clucene in separate processes\n");
  }

  // 6. Show the real on-disk index files.
  std::printf("\n");
  if (want_snii && args.keep_index) print_index_files("SNII", snii_idx.index_files());
  if (want_clu && args.keep_index) print_index_files("CLucene", cl_idx.index_files());

  if (want_snii && want_clu) {
    std::printf("\nresult: %s\n", all_match ? "ALL DOCIDS MATCH" : "DOCID MISMATCH");
    return all_match ? 0 : 1;
  }
  std::printf("\nresult: built engine=%s (consistency check needs --engine both)\n",
              args.engine.c_str());
  return 0;
}

double wall_ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == ' ' || ch == '\t') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// One engine's result for a single query: ascending docids, the three gold I/O
// metrics (captured on a cold run), and per-run wall-clock latency samples.
struct EngineQueryResult {
  std::vector<uint32_t> docids;
  snii::io::IoMetrics metrics;
  std::vector<double> latency_ms;
};

// Runs the query `runs` times on either adapter (each call resets the metered
// reader to a cold cache, so latency includes the physical reads). Returns the
// docids, the last run's I/O metrics, and all latency samples.
template <class Adapter>
EngineQueryResult run_query_n(Adapter& idx, bool is_phrase,
                              const std::string& term,
                              const std::vector<std::string>& words,
                              uint32_t runs) {
  EngineQueryResult r;
  for (uint32_t i = 0; i < runs; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    if (is_phrase) {
      idx.phrase_query(words, &r.docids, &r.metrics);
    } else {
      idx.term_query(term, &r.docids, &r.metrics);
    }
    r.latency_ms.push_back(wall_ms_since(t0));
  }
  return r;
}

// Latency distribution of one query's repeated runs. `cold` is the very FIRST
// run (OS page cache cold); min/median/p90/mean summarise the whole sample so
// warm-cache behaviour is visible alongside the cold first touch.
struct LatStats {
  double cold = 0.0, min = 0.0, median = 0.0, p90 = 0.0, mean = 0.0;
};

LatStats lat_stats(const std::vector<double>& samples) {
  LatStats s;
  if (samples.empty()) return s;
  s.cold = samples.front();  // first run = cold page cache
  std::vector<double> v = samples;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  s.min = v.front();
  s.median = v[n / 2];
  s.p90 = v[static_cast<size_t>(0.9 * (n - 1) + 0.5)];
  double sum = 0.0;
  for (double x : v) sum += x;
  s.mean = sum / static_cast<double>(n);
  return s;
}

void print_latency_row(const char* engine, const std::vector<double>& samples) {
  const LatStats s = lat_stats(samples);
  std::printf("  %-8s latency_ms: cold=%-9.2f min=%-9.2f median=%-9.2f "
              "p90=%-9.2f mean=%-9.2f\n",
              engine, s.cold, s.min, s.median, s.p90, s.mean);
}

// Query-only benchmark over a persisted index dir: opens snii/index.idx and (if
// present) clucene/, then for a set of terms + one phrase asserts SNII docids ==
// CLucene docids and reports query latency plus the design's THREE GOLD METRICS
// (serial I/O rounds, range GETs, bytes read). No import / build.
int run_query_mode(const Args& args) {
  const std::string snii_path = args.query_dir + "/snii/index.idx";
  const std::string clu_dir = args.query_dir + "/clucene";
  const uint32_t R = std::max(args.runs, 5u);  // a small latency distribution

  std::printf("=== query benchmark over persisted index ===\n");
  std::printf("query_dir=%s runs=%u\n", args.query_dir.c_str(), R);

  bench::SniiAdapter snii_idx;
  try {
    snii_idx.open_existing(snii_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: cannot open SNII index %s: %s\n",
                 snii_path.c_str(), e.what());
    return 1;
  }

  // CLucene is optional: it may be absent if its build did not survive this scale.
  bool have_clu = false;
  std::error_code ec;
  if (std::filesystem::is_directory(clu_dir, ec)) {
    for (const auto& f : std::filesystem::directory_iterator(clu_dir, ec)) {
      if (f.is_regular_file(ec)) { have_clu = true; break; }
    }
  }
  bench::CluceneAdapter cl_idx;
  if (have_clu) {
    try {
      cl_idx.open_existing(clu_dir);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "WARNING: cannot open CLucene index %s: %s\n",
                   clu_dir.c_str(), e.what());
      have_clu = false;
    }
  }
  std::printf("engines: SNII=open CLucene=%s\n",
              have_clu ? "open" : "ABSENT (SNII-only metrics)");

  std::vector<std::string> terms =
      split_csv(args.terms.empty()
                    ? "error,failed,checkout,order,currency,rpc,connection,card"
                    : args.terms);
  std::vector<std::string> phrase =
      split_ws(args.phrase.empty() ? "failed to place order" : args.phrase);

  bool all_identical = true;

  auto report = [&](const std::string& label, bool is_phrase,
                    const std::string& term,
                    const std::vector<std::string>& words) {
    EngineQueryResult s, c;
    try {
      s = run_query_n(snii_idx, is_phrase, term, words, R);
      if (have_clu) c = run_query_n(cl_idx, is_phrase, term, words, R);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: query '%s' failed: %s\n", label.c_str(),
                   e.what());
      std::exit(1);
    }
    const bool identical = !have_clu || (s.docids == c.docids);
    if (have_clu && !identical) {
      all_identical = false;
      std::fprintf(stderr, "MISMATCH %s: snii=%zu clucene=%zu\n", label.c_str(),
                   s.docids.size(), c.docids.size());
    }
    std::printf("\n[%s]  hits=%zu  identical=%s\n", label.c_str(),
                s.docids.size(),
                have_clu ? (identical ? "YES" : "NO") : "n/a");
    // Three gold metrics (CLucene first to match the I/O-table style).
    if (have_clu) {
      std::printf("  %-8s gold: serial_rounds=%-5llu range_gets=%-5llu "
                  "bytes=%-11llu\n",
                  "CLucene",
                  static_cast<unsigned long long>(c.metrics.serial_rounds),
                  static_cast<unsigned long long>(c.metrics.range_gets),
                  static_cast<unsigned long long>(c.metrics.total_request_bytes));
    }
    std::printf("  %-8s gold: serial_rounds=%-5llu range_gets=%-5llu "
                "bytes=%-11llu\n",
                "SNII",
                static_cast<unsigned long long>(s.metrics.serial_rounds),
                static_cast<unsigned long long>(s.metrics.range_gets),
                static_cast<unsigned long long>(s.metrics.total_request_bytes));
    if (have_clu) {
      std::printf("  ratio(CL/SNII): serial_rounds=%.2f range_gets=%.2f bytes=%.2f\n",
                  ratio(c.metrics.serial_rounds, s.metrics.serial_rounds),
                  ratio(c.metrics.range_gets, s.metrics.range_gets),
                  ratio(c.metrics.total_request_bytes, s.metrics.total_request_bytes));
    }
    // Full query-latency distribution over the repeated runs (cold = first run).
    if (have_clu) print_latency_row("CLucene", c.latency_ms);
    print_latency_row("SNII", s.latency_ms);
    if (have_clu) {
      const double sl = lat_stats(s.latency_ms).median;
      const double cl = lat_stats(c.latency_ms).median;
      std::printf("  latency ratio(CL/SNII): median=%.2f\n",
                  sl > 0 ? cl / sl : 0.0);
    }
  };

  for (const std::string& t : terms) report("TERM '" + t + "'", false, t, {});
  if (!phrase.empty()) report("PHRASE '" + join_words(phrase) + "'", true,
                              std::string(), phrase);

  if (have_clu) {
    std::printf("\nresult: %s\n",
                all_identical ? "ALL DOCIDS IDENTICAL (SNII == CLucene)"
                              : "DOCID MISMATCH");
    return all_identical ? 0 : 1;
  }
  std::printf("\nresult: SNII-only query metrics (CLucene index absent at this "
              "scale)\n");
  return 0;
}

// Reads the parquet text column and tokenizes it into `*out` (Doris-english,
// `threads` workers). Fills the timing/size out-params. Returns false (after
// printing the error) on failure.
bool load_and_tokenize(const Args& args, uint32_t threads, bench::Corpus* out,
                       uint64_t* raw_bytes, double* tok_cpu, double* tok_wall) {
  std::vector<std::string> bodies;
  *raw_bytes = 0;
  try {
    const auto w0 = std::chrono::steady_clock::now();
    bodies = bench::read_text_column(args.parquet_file, args.text_col, args.docs);
    const double read_wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    for (const std::string& b : bodies) *raw_bytes += b.size();
    std::printf("  read %zu rows (%.1f MiB raw) in %.2f s\n", bodies.size(),
                mib(*raw_bytes), read_wall);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: parquet read failed: %s\n", e.what());
    return false;
  }
  if (bodies.empty()) {
    std::fprintf(stderr, "FATAL: parquet column yielded 0 rows\n");
    return false;
  }
  const double c0 = cpu_seconds();
  const auto t0 = std::chrono::steady_clock::now();
  *out = bench::tokenize_corpus(bodies, threads);
  *tok_cpu = cpu_seconds() - c0;
  *tok_wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

// Runs a query on EVERY shard of one engine and merges the results into the
// global docid space (shard s's local docids are offset by bases[s]). Shards are
// in ascending doc-range order, so the concatenation is already globally
// ascending. The three gold metrics are SUMMED across shards (the query touches
// all shards); latency is the SUM of per-shard wall time per run.
template <class Adapter>
EngineQueryResult query_shards_merged(
    std::vector<std::unique_ptr<Adapter>>& shards,
    const std::vector<uint32_t>& bases, bool is_phrase, const std::string& term,
    const std::vector<std::string>& words, uint32_t runs) {
  EngineQueryResult agg;
  for (uint32_t i = 0; i < runs; ++i) {
    agg.docids.clear();
    agg.metrics = snii::io::IoMetrics{};
    double wall = 0.0;
    for (size_t s = 0; s < shards.size(); ++s) {
      std::vector<uint32_t> d;
      snii::io::IoMetrics m;
      const auto t0 = std::chrono::steady_clock::now();
      if (is_phrase) {
        shards[s]->phrase_query(words, &d, &m);
      } else {
        shards[s]->term_query(term, &d, &m);
      }
      wall += wall_ms_since(t0);
      for (uint32_t x : d) agg.docids.push_back(bases[s] + x);
      agg.metrics.read_at_calls += m.read_at_calls;
      agg.metrics.serial_rounds += m.serial_rounds;
      agg.metrics.range_gets += m.range_gets;
      agg.metrics.remote_bytes += m.remote_bytes;
      agg.metrics.total_request_bytes += m.total_request_bytes;
    }
    agg.latency_ms.push_back(wall);
  }
  return agg;
}

// Sharded large-scale head-to-head: both engines are sharded into contiguous
// doc ranges of `shard_docs` (SNII: one .idx per shard; CLucene: one index dir
// per shard), so each shard stays under CLucene's ~2^30-position ceiling. Queries
// open every shard, offset local docids to the global space, and merge. This is
// the only way to compare the two engines beyond ~95M docs (where a single
// CLucene segment crashes). Both engines' global docids equal the corpus order,
// so SNII == CLucene must hold.
int run_sharded_e2e(const Args& args) {
  const uint32_t threads =
      args.threads != 0 ? args.threads
                        : std::max(1u, std::thread::hardware_concurrency());
  std::string out_dir = args.out_dir.empty()
                            ? std::string("/mnt/disk15/jiangkai/e2e_data/sharded")
                            : args.out_dir;
  std::printf("=== sharded large-scale head-to-head "
              "(SNII shards vs CLucene shards) ===\n");
  std::printf("parquet=%s column=%s max_docs=%u threads=%u shard_docs=%u out=%s\n",
              args.parquet_file.c_str(), args.text_col.c_str(), args.docs, threads,
              args.shard_docs, out_dir.c_str());

  bench::Corpus corpus;
  uint64_t raw_bytes = 0;
  double tok_cpu = 0, tok_wall = 0;
  if (!load_and_tokenize(args, threads, &corpus, &raw_bytes, &tok_cpu, &tok_wall)) {
    return 1;
  }
  uint64_t total_tokens = 0;
  for (const auto& d : corpus.docs) total_tokens += d.size();
  std::printf("  tokenized: docs=%u vocab=%zu tokens=%llu  tok_cpu_s=%.2f "
              "tok_wall_s=%.2f (%.1f MiB/s @ %u threads)\n",
              corpus.doc_count, corpus.vocab.size(),
              static_cast<unsigned long long>(total_tokens), tok_cpu, tok_wall,
              tok_wall > 0 ? mib(raw_bytes) / tok_wall : 0.0, threads);

  // Plan contiguous shards. shard_docs==0 falls back to a single shard.
  const uint32_t M = args.shard_docs != 0 ? args.shard_docs : corpus.doc_count;
  std::vector<uint32_t> bases;
  std::vector<std::pair<uint32_t, uint32_t>> ranges;  // [lo, hi)
  for (uint32_t lo = 0; lo < corpus.doc_count; lo += M) {
    const uint32_t hi = std::min(lo + M, corpus.doc_count);
    bases.push_back(lo);
    ranges.emplace_back(lo, hi);
  }
  const size_t K = ranges.size();
  std::printf("  shards=%zu (%u docs each, last=%u)\n", K, M,
              ranges.back().second - ranges.back().first);

  // Build every shard for both engines, keeping each adapter alive for querying.
  std::vector<std::unique_ptr<bench::SniiAdapter>> snii_shards;
  std::vector<std::unique_ptr<bench::CluceneAdapter>> clu_shards;
  uint64_t snii_bytes = 0, clu_bytes = 0;
  const double bc0 = cpu_seconds();
  const auto bw0 = std::chrono::steady_clock::now();
  try {
    for (size_t k = 0; k < K; ++k) {
      char seg[32];
      std::snprintf(seg, sizeof(seg), "seg_%03zu", k);
      auto sn = std::make_unique<bench::SniiAdapter>();
      apply_spill(*sn, args.spill_mib);
      sn->build_range(out_dir + "/snii/" + seg + ".idx", corpus, ranges[k].first,
                      ranges[k].second, args.keep_index);
      snii_bytes += sn->index_bytes();
      snii_shards.push_back(std::move(sn));

      auto cl = std::make_unique<bench::CluceneAdapter>();
      cl->build_range(out_dir + "/clucene/" + seg, corpus, ranges[k].first,
                      ranges[k].second, args.keep_index);
      clu_bytes += cl->index_bytes();
      clu_shards.push_back(std::move(cl));
      std::printf("  built %s [%u,%u)\n", seg, ranges[k].first, ranges[k].second);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: sharded build failed: %s\n", e.what());
    return 1;
  }
  const double build_cpu = cpu_seconds() - bc0;
  const double build_wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - bw0).count();
  std::printf("  build: SNII=%llu B (%.2f MiB)  CLucene=%llu B (%.2f MiB)  "
              "cpu_s=%.1f wall_s=%.1f  peak_rss=%.1f MiB\n",
              static_cast<unsigned long long>(snii_bytes), mib(snii_bytes),
              static_cast<unsigned long long>(clu_bytes), mib(clu_bytes),
              build_cpu, build_wall, peak_rss_mib());

  // Query suite: merge across shards for both engines; assert identical; report
  // the three gold metrics (summed over shards) and per-query latency.
  const uint32_t R = std::max(args.runs, 5u);
  std::vector<std::string> terms = split_csv(
      args.terms.empty() ? "error,failed,checkout,order,currency,rpc,connection,card"
                         : args.terms);
  std::vector<std::string> phrase =
      split_ws(args.phrase.empty() ? "failed to place order" : args.phrase);
  bool all_identical = true;

  auto report = [&](const std::string& label, bool is_phrase,
                    const std::string& term, const std::vector<std::string>& w) {
    EngineQueryResult s = query_shards_merged(snii_shards, bases, is_phrase, term, w, R);
    EngineQueryResult c = query_shards_merged(clu_shards, bases, is_phrase, term, w, R);
    const bool identical = (s.docids == c.docids);
    if (!identical) {
      all_identical = false;
      std::fprintf(stderr, "MISMATCH %s: snii=%zu clucene=%zu\n", label.c_str(),
                   s.docids.size(), c.docids.size());
    }
    std::printf("\n[%s]  hits=%zu  identical=%s\n", label.c_str(), s.docids.size(),
                identical ? "YES" : "NO");
    std::printf("  CLucene gold: serial_rounds=%-6llu range_gets=%-6llu bytes=%-12llu\n",
                static_cast<unsigned long long>(c.metrics.serial_rounds),
                static_cast<unsigned long long>(c.metrics.range_gets),
                static_cast<unsigned long long>(c.metrics.total_request_bytes));
    std::printf("  SNII    gold: serial_rounds=%-6llu range_gets=%-6llu bytes=%-12llu\n",
                static_cast<unsigned long long>(s.metrics.serial_rounds),
                static_cast<unsigned long long>(s.metrics.range_gets),
                static_cast<unsigned long long>(s.metrics.total_request_bytes));
    std::printf("  ratio(CL/SNII): serial_rounds=%.2f range_gets=%.2f bytes=%.2f\n",
                ratio(c.metrics.serial_rounds, s.metrics.serial_rounds),
                ratio(c.metrics.range_gets, s.metrics.range_gets),
                ratio(c.metrics.total_request_bytes, s.metrics.total_request_bytes));
    print_latency_row("CLucene", c.latency_ms);
    print_latency_row("SNII", s.latency_ms);
    const double sl = lat_stats(s.latency_ms).median, clm = lat_stats(c.latency_ms).median;
    std::printf("  latency ratio(CL/SNII): median=%.2f\n", sl > 0 ? clm / sl : 0.0);
  };

  for (const std::string& t : terms) report("TERM '" + t + "'", false, t, {});
  if (!phrase.empty())
    report("PHRASE '" + join_words(phrase) + "'", true, std::string(), phrase);

  std::printf("\nresult: %s (shards=%zu, docs=%u)\n",
              all_identical ? "ALL DOCIDS IDENTICAL (SNII == CLucene)"
                            : "DOCID MISMATCH",
              K, corpus.doc_count);
  return all_identical ? 0 : 1;
}

// One resolved benchmark scenario. `words` holds phrase tokens (kPhrase) or the
// term list (kAnd / kOr); `term` holds the single term (kTerm).
enum class SKind { kTerm, kPhrase, kAnd, kOr, kMatchAll };
struct Scenario {
  std::string id;
  SKind kind = SKind::kTerm;
  std::string term;
  std::vector<std::string> words;
};

// Resolves the enriched catalog from the corpus via the df-bucket selectors: a
// calibrated term df sweep, a phrase length sweep, boolean AND (mixed-df, the
// driver+covering-window claim), boolean OR (df mix), and match-all.
std::vector<Scenario> resolve_scenarios(const bench::Corpus& c) {
  std::vector<Scenario> out;
  auto add_term = [&](const std::string& id, uint32_t tid) {
    if (tid < c.vocab.size()) out.push_back({id, SKind::kTerm, c.vocab[tid], {}});
  };
  add_term("TERM-very-high", bench::term_in_df_bucket(c, 0.5, 1.0));
  add_term("TERM-high", bench::term_in_df_bucket(c, 0.1, 0.5));
  add_term("TERM-mid", bench::mid_df_term(c));
  add_term("TERM-low", bench::low_df_term(c));
  add_term("TERM-df1", bench::df1_term(c));
  out.push_back({"TERM-absent", SKind::kTerm, bench::absent_token(c), {}});  // df=0
  for (uint32_t n : {2u, 3u, 5u, 8u}) {
    std::vector<std::string> ph = bench::extract_phrase(c, n);
    if (!ph.empty())
      out.push_back({"PHRASE-" + std::to_string(n), SKind::kPhrase, "", ph});
  }
  // AND-HIGH-LOW: very-high-df stopword AND a co-occurring rare term (driver=rare).
  const auto pr = bench::cooccurring_pair(c, 0.5, 1.0, 0.0, 0.01);
  if (pr.first != pr.second) {
    out.push_back({"AND-HIGH-LOW", SKind::kAnd, "",
                   {c.vocab[pr.first], c.vocab[pr.second]}});
  }
  // AND-DENSE (honest envelope): two HIGH-df co-occurring terms -- SNII's worst AND
  // (no rare driver, both near-full postings read; bytes converge to/below CLucene).
  const auto dn = bench::cooccurring_pair(c, 0.5, 1.0, 0.2, 1.0);
  if (dn.first != dn.second) {
    out.push_back({"AND-DENSE", SKind::kAnd, "",
                   {c.vocab[dn.first], c.vocab[dn.second]}});
  }
  // PHRASE-REVERSED (honest envelope): reverse a real 3-gram -> usually empty.
  // Stresses the empty-result short-circuit; gate stays SNII == CLucene either way.
  {
    std::vector<std::string> ph = bench::extract_phrase(c, 3);
    if (ph.size() == 3) {
      std::reverse(ph.begin(), ph.end());
      out.push_back({"PHRASE-REVERSED", SKind::kPhrase, "", ph});
    }
  }
  // OR-MIXED-DF: union over a high + mid + low term (distinct).
  {
    std::vector<uint32_t> ids = {bench::term_in_df_bucket(c, 0.1, 0.5),
                                 bench::mid_df_term(c), bench::low_df_term(c)};
    std::vector<std::string> terms;
    for (uint32_t id : ids)
      if (id < c.vocab.size() &&
          std::find(terms.begin(), terms.end(), c.vocab[id]) == terms.end())
        terms.push_back(c.vocab[id]);
    if (terms.size() >= 2) out.push_back({"OR-MIXED-DF", SKind::kOr, "", terms});
  }
  out.push_back({"MATCH-ALL", SKind::kMatchAll, "", {}});
  return out;
}

// One query of a scenario, dispatched on kind (shared by single + concurrent runs).
template <class Adapter>
void run_one(Adapter& idx, const Scenario& s, std::vector<uint32_t>* docids,
             snii::io::IoMetrics* metrics) {
  switch (s.kind) {
    case SKind::kTerm: idx.term_query(s.term, docids, metrics); break;
    case SKind::kPhrase: idx.phrase_query(s.words, docids, metrics); break;
    case SKind::kAnd: idx.boolean_and(s.words, docids, metrics); break;
    case SKind::kOr: idx.boolean_or(s.words, docids, metrics); break;
    case SKind::kMatchAll: idx.match_all(docids, metrics); break;
  }
}

// Runs one scenario `runs` times on an adapter, dispatching on its kind.
template <class Adapter>
EngineQueryResult run_scenario_on(Adapter& idx, const Scenario& s, uint32_t runs) {
  EngineQueryResult r;
  for (uint32_t i = 0; i < runs; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    run_one(idx, s, &r.docids, &r.metrics);
    r.latency_ms.push_back(wall_ms_since(t0));
  }
  return r;
}

// Aggregate result of a concurrent throughput pass.
struct ConcResult {
  uint32_t threads = 0;
  uint64_t queries = 0;   // post-warmup queries counted toward latency/QPS
  double wall_s = 0.0;
  double qps = 0.0;
  double p50 = 0.0, p90 = 0.0, p99 = 0.0, mean = 0.0;
};

// Nearest-rank percentile (idx = round(p*(n-1))), matching lat_stats' convention.
double pct(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  const size_t i = static_cast<size_t>(p * (sorted.size() - 1) + 0.5);
  return sorted[i];
}

// Runs `scenarios` round-robin on `threads` worker threads for `seconds` wall-clock,
// each thread owning an INDEPENDENT adapter from make_adapter() (no shared mutable
// state -- verified race-free). `run_query(adapter, scenario, &docids, &scratch)`
// dispatches one query (so the local and S3 paths reuse the same driver). The first
// `warmup` queries per thread are discarded. Returns throughput (QPS) + tail latency.
template <class MakeAdapter, class RunFn>
ConcResult run_concurrent(MakeAdapter make_adapter, RunFn run_query,
                          const std::vector<Scenario>& scs, uint32_t threads,
                          double seconds, uint32_t warmup) {
  std::atomic<bool> go{false}, stop{false};
  std::vector<std::vector<double>> per_thread(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (uint32_t t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      auto adapter = make_adapter();  // own reader chain (per-thread isolation)
      std::vector<double>& lat = per_thread[t];
      lat.reserve(16384);
      std::vector<uint32_t> docids;
      snii::io::IoMetrics scratch;
      size_t si = t;  // round-robin offset so threads spread across scenarios
      while (!go.load(std::memory_order_acquire)) {
      }
      while (!stop.load(std::memory_order_relaxed)) {
        const Scenario& s = scs[si++ % scs.size()];
        const auto t0 = std::chrono::steady_clock::now();
        run_query(*adapter, s, &docids, &scratch);
        lat.push_back(wall_ms_since(t0));  // record all; warmup trimmed at aggregation
      }
    });
  }
  const auto wall0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& w : workers) w.join();
  const double wall = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - wall0).count();

  // Trim the first `warmup` queries per thread (cold start), but never zero out a
  // thread: a slow engine that ran <= warmup queries keeps its last sample so QPS /
  // tail latency are still reported instead of collapsing to 0.
  std::vector<double> all;
  for (auto& v : per_thread) {
    const size_t skip = v.size() > warmup ? warmup : (v.empty() ? 0 : v.size() - 1);
    for (size_t i = skip; i < v.size(); ++i) all.push_back(v[i]);
  }
  std::sort(all.begin(), all.end());
  ConcResult r;
  r.threads = threads;
  r.queries = all.size();
  r.wall_s = wall;
  r.qps = wall > 0 ? static_cast<double>(all.size()) / wall : 0.0;
  r.p50 = pct(all, 0.50);
  r.p90 = pct(all, 0.90);
  r.p99 = pct(all, 0.99);
  double sum = 0.0;
  for (double v : all) sum += v;
  r.mean = all.empty() ? 0.0 : sum / static_cast<double>(all.size());
  return r;
}

void print_conc_row(const char* engine, const ConcResult& c) {
  std::printf("  %-8s threads=%-3u QPS=%-9.0f queries=%-8llu p50=%.3f p90=%.3f "
              "p99=%.3f mean=%.3f ms\n",
              engine, c.threads, c.qps,
              static_cast<unsigned long long>(c.queries), c.p50, c.p90, c.p99, c.mean);
}

#ifdef SNII_WITH_S3
// Concurrent S3 throughput: build/upload already done; each worker opens its OWN
// S3 reader chain over the uploaded index (open_uploaded). term/phrase scenarios
// only (the OSS adapters expose term_query/phrase_query). The wall-clock is real
// OSS round-trips, where SNII's fewer serial rounds drive the QPS / tail advantage.
void run_oss_concurrent(const Args& args, const bench::Corpus& corpus,
                        const snii::io::S3Config& cfg, const std::string& snii_key,
                        const std::vector<std::string>& clu_names) {
  std::vector<Scenario> all = resolve_scenarios(corpus);
  std::vector<Scenario> scs;  // term + phrase only (OSS adapter query surface)
  for (const Scenario& s : all)
    if (s.kind == SKind::kTerm || s.kind == SKind::kPhrase) scs.push_back(s);
  if (scs.empty()) return;

  auto make_snii = [&cfg, snii_key]() {
    auto a = std::make_unique<bench::SniiOssAdapter>();
    a->open_uploaded(snii_key, cfg);
    return a;
  };
  auto make_clu = [&cfg, clu_names]() {
    auto a = std::make_unique<bench::CluceneOssAdapter>();
    a->open_uploaded(cfg, clu_names);
    return a;
  };
  auto run_tp = [](auto& a, const Scenario& s, std::vector<uint32_t>* d,
                   snii::io::IoMetrics* m) {
    if (s.kind == SKind::kPhrase) a.phrase_query(s.words, d, m);
    else a.term_query(s.term, d, m);
  };

  const double secs = args.concurrency_seconds;
  const uint32_t W = args.concurrency;
  std::printf("\n=== concurrent throughput (REAL OSS, %.0fs/pass, warmup=%u/thread, "
              "term+phrase) ===\n", secs, args.concurrency_warmup);
  std::vector<uint32_t> levels = {1u};
  if (W != 1) levels.push_back(W);
  for (uint32_t N : levels) {
    print_conc_row("CLucene", run_concurrent(make_clu, run_tp, scs, N, secs,
                                             args.concurrency_warmup));
    print_conc_row("SNII", run_concurrent(make_snii, run_tp, scs, N, secs,
                                          args.concurrency_warmup));
  }
}
#endif  // SNII_WITH_S3

void report_scenario(const Scenario& s, const EngineQueryResult& sn,
                     const EngineQueryResult& cl, bool* all_identical) {
  const bool id = (sn.docids == cl.docids);
  if (!id) *all_identical = false;
  std::printf("\n[%s] hits=%zu identical=%s\n", s.id.c_str(), sn.docids.size(),
              id ? "YES" : "NO");
  std::printf("  CLucene gold: serial_rounds=%-5llu range_gets=%-5llu bytes=%-11llu\n",
              static_cast<unsigned long long>(cl.metrics.serial_rounds),
              static_cast<unsigned long long>(cl.metrics.range_gets),
              static_cast<unsigned long long>(cl.metrics.total_request_bytes));
  std::printf("  SNII    gold: serial_rounds=%-5llu range_gets=%-5llu bytes=%-11llu\n",
              static_cast<unsigned long long>(sn.metrics.serial_rounds),
              static_cast<unsigned long long>(sn.metrics.range_gets),
              static_cast<unsigned long long>(sn.metrics.total_request_bytes));
  std::printf("  ratio(CL/SNII): serial_rounds=%.2f range_gets=%.2f bytes=%.2f\n",
              ratio(cl.metrics.serial_rounds, sn.metrics.serial_rounds),
              ratio(cl.metrics.range_gets, sn.metrics.range_gets),
              ratio(cl.metrics.total_request_bytes, sn.metrics.total_request_bytes));
  print_latency_row("CLucene", cl.latency_ms);
  print_latency_row("SNII", sn.latency_ms);
  const double sl = lat_stats(sn.latency_ms).median;
  const double clm = lat_stats(cl.latency_ms).median;
  std::printf("  latency ratio(CL/SNII): median=%.2f\n", sl > 0 ? clm / sl : 0.0);
}

// SNII BM25 scoring path-equivalence (SCORE-PATH-EQUIV): builds a scoring index and
// runs exhaustive / WAND / selective-WAND top-K for high-idf + low-idf term mixes.
// All three MUST return the identical top-K (the design invariant); the I/O metrics
// show the window block-max pruning benefit (bytes: exhaustive >= wand >= selective).
// CLucene head-to-head is intentionally omitted: its searchable Similarity is TF-IDF
// (DefaultSimilarity); BM25 lives only in the index-side BlockMaxBM25 path, so a fair
// BM25 ranking comparison needs the Doris block-max query path (a separate follow-up).
int run_scoring_mode(const Args& args) {
  const uint32_t threads =
      args.threads != 0 ? args.threads
                        : std::max(1u, std::thread::hardware_concurrency());
  bench::Corpus corpus;
  if (!args.parquet_file.empty()) {
    uint64_t raw = 0;
    double tc = 0, tw = 0;
    if (!load_and_tokenize(args, threads, &corpus, &raw, &tc, &tw)) return 1;
  } else {
    corpus = bench::generate(args.docs, args.vocab, args.zipf, args.doclen, args.seed);
  }
  std::printf("scoring corpus: docs=%u vocab=%zu\n", corpus.doc_count,
              corpus.vocab.size());

  bench::SniiAdapter idx;
  idx.set_scoring(true);
  apply_spill(idx, args.spill_mib);
  const std::string out =
      args.out_dir.empty() ? std::string("/mnt/disk15/jiangkai/e2e_data/scoring")
                           : args.out_dir;
  try {
    idx.build_at(out + "/snii/index.idx", corpus, args.keep_index);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: scoring build failed: %s\n", e.what());
    return 1;
  }

  auto vocab = [&](uint32_t t) {
    return t < corpus.vocab.size() ? corpus.vocab[t] : std::string();
  };
  const std::string hi = vocab(bench::term_in_df_bucket(corpus, 0.1, 0.5));
  const std::string mid = vocab(bench::mid_df_term(corpus));
  const std::string lo = vocab(bench::low_df_term(corpus));
  struct SQ {
    std::string id;
    std::vector<std::string> terms;
  };
  std::vector<SQ> queries;
  if (!hi.empty() && !lo.empty()) queries.push_back({"SCORE-high+low", {hi, lo}});
  if (!hi.empty() && !mid.empty() && !lo.empty())
    queries.push_back({"SCORE-high+mid+low", {hi, mid, lo}});

  using SP = bench::SniiAdapter::ScorePath;
  using Hit = bench::SniiAdapter::ScoredHit;
  auto eq = [](const std::vector<Hit>& a, const std::vector<Hit>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (a[i].docid != b[i].docid || a[i].score != b[i].score) return false;
    return true;
  };
  auto row = [](const char* name, const snii::io::IoMetrics& m) {
    std::printf("  %-11s rounds=%-3llu gets=%-3llu bytes=%llu\n", name,
                static_cast<unsigned long long>(m.serial_rounds),
                static_cast<unsigned long long>(m.range_gets),
                static_cast<unsigned long long>(m.total_request_bytes));
  };

  bool all_ok = true;
  for (const SQ& q : queries) {
    for (uint32_t k : {10u, 100u}) {
      snii::io::IoMetrics me, mw, ms;
      std::vector<Hit> ex, wa, ws;
      try {
        idx.score_query(q.terms, k, SP::kExhaustive, &ex, &me);
        idx.score_query(q.terms, k, SP::kWand, &wa, &mw);
        idx.score_query(q.terms, k, SP::kWandSelective, &ws, &ms);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: score_query %s: %s\n", q.id.c_str(), e.what());
        return 1;
      }
      const bool identical = eq(ex, wa) && eq(ex, ws);
      if (!identical) all_ok = false;
      std::printf("\n[%s k=%u] topK=%zu path-equiv(exh==wand==sel)=%s\n",
                  q.id.c_str(), k, ex.size(), identical ? "YES" : "NO");
      row("exhaustive", me);
      row("wand", mw);
      row("wand_sel", ms);
      std::printf("  selective prune vs exhaustive: bytes=%.2fx rounds=%.2fx\n",
                  ratio(me.total_request_bytes, ms.total_request_bytes),
                  ratio(me.serial_rounds, ms.serial_rounds));
    }
  }
  std::printf("\nresult: %s (SNII scoring path-equivalence)\n",
              all_ok ? "ALL PATHS IDENTICAL (exhaustive == wand == wand_selective)"
                     : "PATH MISMATCH");
  return all_ok ? 0 : 1;
}

// Prefix + MATCH_PHRASE_PREFIX head-to-head: builds both engines (tokenized) and
// compares prefix_query (SNII ordered term enumeration + union vs CLucene
// PrefixQuery) and match_phrase_prefix (union of phrase(fixed + expansion) over the
// prefix's terms, both engines), with the three gold metrics + latency.
int run_prefix_mode(const Args& args) {
  const uint32_t threads =
      args.threads != 0 ? args.threads
                        : std::max(1u, std::thread::hardware_concurrency());
  bench::Corpus corpus;
  if (!args.parquet_file.empty()) {
    uint64_t raw = 0;
    double tc = 0, tw = 0;
    if (!load_and_tokenize(args, threads, &corpus, &raw, &tc, &tw)) return 1;
  } else {
    corpus = bench::generate(args.docs, args.vocab, args.zipf, args.doclen, args.seed);
  }
  std::printf("prefix corpus: docs=%u vocab=%zu\n", corpus.doc_count,
              corpus.vocab.size());

  bench::SniiAdapter snii_idx;
  bench::CluceneAdapter cl_idx;
  apply_spill(snii_idx, args.spill_mib);
  apply_spill(cl_idx, args.spill_mib);
  const std::string out =
      args.out_dir.empty() ? std::string("/mnt/disk15/jiangkai/e2e_data/prefix")
                           : args.out_dir;
  try {
    snii_idx.build_at(out + "/snii/index.idx", corpus, args.keep_index);
    cl_idx.build_at(out + "/clucene", corpus, args.keep_index);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: prefix build failed: %s\n", e.what());
    return 1;
  }

  // Choose the SHORTEST prefix of a term whose expansion count is bounded (<=200),
  // so the comparison stays meaningful and CLucene's PrefixQuery stays under its
  // BooleanQuery clause cap (a broad prefix would otherwise throw "Too Many Clauses").
  auto bounded_prefix = [&](const std::string& w) -> std::string {
    for (size_t L = 1; L <= w.size(); ++L) {
      std::string p = w.substr(0, L);
      if (snii_idx.enumerate_prefix(p).size() <= 200) return p;
    }
    return w;
  };
  // Use real content words (a 2-gram) so the prefixes are meaningful word stems.
  const std::vector<std::string> bigram = bench::extract_phrase(corpus, 2);
  const std::string prefix = bigram.size() == 2 ? bounded_prefix(bigram[0])
                                                : std::string();

  bool all_identical = true;
  snii::io::IoMetrics ms, mc;
  auto compare = [&](const char* label, std::vector<uint32_t>& a,
                     std::vector<uint32_t>& b) {
    const bool id = (a == b);
    if (!id) all_identical = false;
    std::printf("\n[%s] SNII=%zu CLucene=%zu %s\n", label, a.size(), b.size(),
                id ? "IDENTICAL" : "MISMATCH");
    std::printf("  CLucene gold: serial_rounds=%llu range_gets=%llu bytes=%llu\n",
                static_cast<unsigned long long>(mc.serial_rounds),
                static_cast<unsigned long long>(mc.range_gets),
                static_cast<unsigned long long>(mc.total_request_bytes));
    std::printf("  SNII    gold: serial_rounds=%llu range_gets=%llu bytes=%llu\n",
                static_cast<unsigned long long>(ms.serial_rounds),
                static_cast<unsigned long long>(ms.range_gets),
                static_cast<unsigned long long>(ms.total_request_bytes));
  };

  if (!prefix.empty()) {
    std::vector<uint32_t> sd, cd;
    snii_idx.prefix_query(prefix, &sd, &ms);
    cl_idx.prefix_query(prefix, &cd, &mc);
    const std::vector<std::string> exp = snii_idx.enumerate_prefix(prefix);
    std::printf("prefix='%s' (%zu expansion terms)", prefix.c_str(), exp.size());
    compare(("PREFIX '" + prefix + "*'").c_str(), sd, cd);

    if (bigram.size() == 2) {
      const std::string p2 = bounded_prefix(bigram[1]);
      const std::vector<std::string> exp2 = snii_idx.enumerate_prefix(p2);
      const std::vector<std::string> fixed = {bigram[0]};
      std::vector<uint32_t> sp, cp;
      snii_idx.phrase_prefix_query(fixed, exp2, &sp, &ms);
      cl_idx.phrase_prefix_query(fixed, exp2, &cp, &mc);
      std::printf("phrase-prefix '%s %s*' (%zu expansions)", bigram[0].c_str(),
                  p2.c_str(), exp2.size());
      compare(("MATCH_PHRASE_PREFIX '" + bigram[0] + " " + p2 + "*'").c_str(), sp, cp);
    }
  }

  std::printf("\nresult: %s (prefix / match_phrase_prefix)\n",
              all_identical ? "ALL DOCIDS IDENTICAL (SNII == CLucene)"
                            : "DOCID MISMATCH");
  return all_identical ? 0 : 1;
}

// SNII multi-index WRITE test: one container holds multiple logical indexes
// (tokenized Body + keyword ServiceName). Verifies each field, queried from the
// multi-container, returns EXACTLY the same docids as a single-field index built
// alone (multi-write does not corrupt any index), and reports the container size
// vs the sum of single indexes. Corpus from parquet (Body + ServiceName) or synthetic.
int run_multi_index_mode(const Args& args) {
  const uint32_t threads =
      args.threads != 0 ? args.threads
                        : std::max(1u, std::thread::hardware_concurrency());
  bench::Corpus body, service;
  if (!args.parquet_file.empty()) {
    std::vector<std::string> bodies, svcs;
    try {
      bodies = bench::read_text_column(args.parquet_file, "Body", args.docs);
      svcs = bench::read_text_column(args.parquet_file, "ServiceName", args.docs);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: parquet read failed: %s\n", e.what());
      return 1;
    }
    const size_t n = std::min(bodies.size(), svcs.size());
    bodies.resize(n);
    svcs.resize(n);
    for (std::string& v : svcs)
      for (char& ch : v)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    body = bench::tokenize_corpus(bodies, threads);
    service = bench::keyword_corpus(svcs);
  } else {
    body = bench::generate(args.docs, args.vocab, args.zipf, args.doclen, args.seed);
    static const char* kSvc[] = {"frontend", "cartservice", "checkoutservice",
                                 "paymentservice", "shippingservice"};
    std::vector<std::string> svcs(body.doc_count);
    for (uint32_t i = 0; i < body.doc_count; ++i) svcs[i] = kSvc[i % 5];
    service = bench::keyword_corpus(svcs);
  }
  std::printf("multi-index corpus: docs=%u body_vocab=%zu service_vocab=%zu\n",
              body.doc_count, body.vocab.size(), service.vocab.size());

  const std::string out =
      args.out_dir.empty() ? std::string("/mnt/disk15/jiangkai/e2e_data/multi")
                           : args.out_dir;
  bench::SniiAdapter multi;
  apply_spill(multi, args.spill_mib);
  std::vector<bench::SniiAdapter::LogicalSpec> specs = {
      {&body, "body", /*docs_only=*/false},
      {&service, "service", /*docs_only=*/true}};
  bench::SniiAdapter single_body, single_service;
  single_service.set_docs_only(true);
  apply_spill(single_body, args.spill_mib);
  apply_spill(single_service, args.spill_mib);
  try {
    multi.build_multi(out + "/multi.idx", specs, args.keep_index);
    single_body.build_at(out + "/single_body.idx", body, args.keep_index);
    single_service.build_at(out + "/single_service.idx", service, args.keep_index);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: multi-index build failed: %s\n", e.what());
    return 1;
  }

  // Self-consistency: each field queried from the multi-container == single-built.
  bool ok = true;
  snii::io::IoMetrics m;
  auto cmp = [&](const char* label, std::vector<uint32_t>& a,
                 std::vector<uint32_t>& b) {
    const bool eq = (a == b);
    if (!eq) ok = false;
    std::printf("  [%s] multi=%zu single=%zu %s\n", label, a.size(), b.size(),
                eq ? "IDENTICAL" : "MISMATCH");
  };
  // body field (tokenized): a high-df term + a real phrase.
  const uint32_t bt = bench::term_in_df_bucket(body, 0.1, 0.5);
  if (bt < body.vocab.size()) {
    std::vector<uint32_t> a, b;
    multi.open_logical(1, "body");
    multi.term_query(body.vocab[bt], &a, &m);
    single_body.term_query(body.vocab[bt], &b, &m);
    cmp("body-term", a, b);
    std::vector<std::string> ph = bench::extract_phrase(body, 3);
    if (!ph.empty()) {
      std::vector<uint32_t> pa, pb;
      multi.phrase_query(ph, &pa, &m);
      single_body.phrase_query(ph, &pb, &m);
      cmp("body-phrase", pa, pb);
    }
  }
  // service field (keyword docs-only): a high-df value.
  const uint32_t st = bench::term_in_df_bucket(service, 0.1, 1.0);
  if (st < service.vocab.size()) {
    std::vector<uint32_t> a, b;
    multi.open_logical(2, "service");
    multi.term_query(service.vocab[st], &a, &m);
    single_service.term_query(service.vocab[st], &b, &m);
    cmp("service-term", a, b);
  }

  const uint64_t mb = multi.index_bytes();
  const uint64_t sb = single_body.index_bytes();
  const uint64_t ss = single_service.index_bytes();
  std::printf("index bytes: multi=%llu  single(body=%llu + service=%llu = %llu)  multi/sum=%.3f\n",
              static_cast<unsigned long long>(mb),
              static_cast<unsigned long long>(sb),
              static_cast<unsigned long long>(ss),
              static_cast<unsigned long long>(sb + ss),
              (sb + ss) ? static_cast<double>(mb) / static_cast<double>(sb + ss) : 0.0);
  std::printf("\nresult: %s (SNII multi-index write self-consistency)\n",
              ok ? "ALL FIELDS IDENTICAL (multi == single)" : "FIELD MISMATCH");
  return ok ? 0 : 1;
}

// Non-tokenized (keyword) index comparison: each document's WHOLE field value is
// one term. Both engines build a DOCS-ONLY index (SNII kDocsOnly; CLucene
// omitTermFreqAndPositions) and answer exact-value lookups. Values are lowercased
// single tokens (e.g. OTel ServiceName / SeverityText / TraceId) so the whole value
// survives CLucene's SimpleAnalyzer as one term -- a faithful docs-only PERFORMANCE
// comparison. (Case-sensitive multi-word keyword would need CLucene untokenized
// fields, a separate build path; out of scope here.)
int run_keyword_mode(const Args& args) {
  std::vector<std::string> values;
  if (!args.parquet_file.empty()) {
    try {
      values = bench::read_text_column(args.parquet_file, args.text_col, args.docs);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: parquet read failed: %s\n", e.what());
      return 1;
    }
  } else {
    static const char* kServices[] = {
        "frontend", "cartservice", "checkoutservice", "paymentservice",
        "shippingservice", "emailservice", "currencyservice",
        "recommendationservice", "adservice", "productcatalogservice"};
    const uint32_t n = args.docs ? args.docs : 100000;
    values.reserve(n);
    for (uint32_t i = 0; i < n; ++i) values.push_back(kServices[i % 10]);
    for (uint32_t i = 0; i < n / 1000; ++i) values[i] = "trace" + std::to_string(i);
  }
  if (values.empty()) {
    std::fprintf(stderr, "FATAL: keyword column yielded 0 rows\n");
    return 1;
  }
  // Lowercase whole values to match CLucene's SimpleAnalyzer (single-token keyword).
  for (std::string& v : values)
    for (char& ch : v)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

  bench::Corpus corpus = bench::keyword_corpus(values);
  std::printf("keyword corpus: docs=%u distinct=%zu (col=%s)\n", corpus.doc_count,
              corpus.vocab.size(), args.text_col.c_str());

  bench::SniiAdapter snii_idx;
  bench::CluceneAdapter cl_idx;
  snii_idx.set_docs_only(true);
  cl_idx.set_docs_only(true);
  apply_spill(snii_idx, args.spill_mib);
  apply_spill(cl_idx, args.spill_mib);
  const std::string out =
      args.out_dir.empty() ? std::string("/mnt/disk15/jiangkai/e2e_data/keyword")
                           : args.out_dir;
  try {
    snii_idx.build_at(out + "/snii/index.idx", corpus, args.keep_index);
    cl_idx.build_at(out + "/clucene", corpus, args.keep_index);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: keyword build failed: %s\n", e.what());
    return 1;
  }
  std::printf("keyword index bytes (docs-only): SNII=%llu CLucene=%llu (CL/SNII=%.2f)\n",
              static_cast<unsigned long long>(snii_idx.index_bytes()),
              static_cast<unsigned long long>(cl_idx.index_bytes()),
              ratio(cl_idx.index_bytes(), snii_idx.index_bytes()));

  std::vector<Scenario> scenarios;
  auto add = [&](const std::string& id, uint32_t tid) {
    if (tid < corpus.vocab.size())
      scenarios.push_back({id, SKind::kTerm, corpus.vocab[tid], {}});
  };
  add("KW-very-high", bench::term_in_df_bucket(corpus, 0.5, 1.0));
  add("KW-high", bench::term_in_df_bucket(corpus, 0.1, 0.5));
  add("KW-mid", bench::mid_df_term(corpus));
  add("KW-df1", bench::df1_term(corpus));
  scenarios.push_back({"KW-absent", SKind::kTerm, bench::absent_token(corpus), {}});

  const uint32_t R = std::max(args.runs, 5u);
  std::printf("=== keyword (non-tokenized, docs-only) exact-query suite (%zu, runs=%u) ===\n",
              scenarios.size(), R);
  bool all_identical = true;
  for (const Scenario& s : scenarios) {
    EngineQueryResult sn, cl;
    try {
      sn = run_scenario_on(snii_idx, s, R);
      cl = run_scenario_on(cl_idx, s, R);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: keyword scenario %s: %s\n", s.id.c_str(), e.what());
      return 1;
    }
    report_scenario(s, sn, cl, &all_identical);
  }
  std::printf("\nresult: %s (keyword, %zu scenarios)\n",
              all_identical ? "ALL DOCIDS IDENTICAL (SNII == CLucene)"
                            : "DOCID MISMATCH",
              scenarios.size());
  return all_identical ? 0 : 1;
}

// Enriched scenario suite (local): build both engines from the corpus, resolve the
// df-calibrated catalog, run every scenario on both, compare docids + the three
// gold metrics + latency. Corpus from parquet (--parquet-file) or synthetic.
int run_scenario_mode(const Args& args) {
  const uint32_t threads =
      args.threads != 0 ? args.threads
                        : std::max(1u, std::thread::hardware_concurrency());
  bench::Corpus corpus;
  if (!args.parquet_file.empty()) {
    uint64_t raw = 0;
    double tc = 0, tw = 0;
    if (!load_and_tokenize(args, threads, &corpus, &raw, &tc, &tw)) return 1;
    std::printf("scenario corpus: parquet docs=%u vocab=%zu\n", corpus.doc_count,
                corpus.vocab.size());
  } else {
    corpus = bench::generate(args.docs, args.vocab, args.zipf, args.doclen, args.seed);
    std::printf("scenario corpus: synthetic docs=%u vocab=%zu\n", corpus.doc_count,
                corpus.vocab.size());
  }

  bench::SniiAdapter snii_idx;
  bench::CluceneAdapter cl_idx;
  apply_spill(snii_idx, args.spill_mib);
  apply_spill(cl_idx, args.spill_mib);
  const std::string out =
      args.out_dir.empty() ? std::string("/mnt/disk15/jiangkai/e2e_data/scenario")
                           : args.out_dir;
  try {
    snii_idx.build_at(out + "/snii/index.idx", corpus, args.keep_index);
    cl_idx.build_at(out + "/clucene", corpus, args.keep_index);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: scenario build failed: %s\n", e.what());
    return 1;
  }

  const std::vector<Scenario> scenarios = resolve_scenarios(corpus);
  const uint32_t R = std::max(args.runs, 5u);
  std::printf("=== scenario suite (local cost-model + wall, %zu scenarios, runs=%u) ===\n",
              scenarios.size(), R);
  bool all_identical = true;

  // BENCH.4: when --bench-out is set, gate each scenario against the declarative
  // threshold manifest (per-scale, local surface) and emit one JSONL row each.
  // overall_pass = docids_match AND every manifest metric verdict pass; the run
  // exits non-zero iff any row fails. The JSONL is the reproducible CI artifact.
  const bool gated = !args.bench_out.empty();
  const std::string rev = gated ? git_rev() : std::string();
  std::vector<bench::BenchRow> rows;

  for (const Scenario& s : scenarios) {
    EngineQueryResult sn, cl;
    try {
      sn = run_scenario_on(snii_idx, s, R);
      cl = run_scenario_on(cl_idx, s, R);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: scenario %s: %s\n", s.id.c_str(), e.what());
      return 1;
    }
    report_scenario(s, sn, cl, &all_identical);
    if (gated) {
      const bool docids_match = (sn.docids == cl.docids);
      rows.push_back(bench::build_row(s.id, "local", corpus.doc_count, args.seed,
                                      rev, sn.docids.size(), docids_match,
                                      cl.metrics, sn.metrics));
    }
  }

  if (gated) {
    std::ofstream os(args.bench_out, std::ios::out | std::ios::trunc);
    if (!os) {
      std::fprintf(stderr, "FATAL: cannot open --bench-out path: %s\n",
                   args.bench_out.c_str());
      return 1;  // CI artifact must never fail silently.
    }
    for (const bench::BenchRow& row : rows) bench::write_jsonl(os, row);
    os.flush();
    if (!os) {
      std::fprintf(stderr, "FATAL: write to --bench-out failed: %s\n",
                   args.bench_out.c_str());
      return 1;
    }
    const bool pass = bench::all_passed(rows);
    std::printf("\n=== gate: %zu rows, verdict=%s -> %s ===\n", rows.size(),
                pass ? "PASS" : "FAIL", args.bench_out.c_str());
    if (!pass) {
      std::fprintf(stderr, "BENCH GATE FAILED: one or more scenarios breached "
                           "their declared threshold (see %s)\n",
                   args.bench_out.c_str());
      return 1;  // non-zero exit on any threshold breach.
    }
  }

  // Concurrent throughput matrix: single (N=1) vs concurrent (N threads), per
  // engine, over the SAME scenario set. Each worker owns an independent adapter
  // (open_existing over the just-built index) -- verified race-free. The single-
  // thread docid-identity gate above must have passed first.
  if (args.concurrency != 0) {
    const std::string snii_path = out + "/snii/index.idx";
    const std::string clu_path = out + "/clucene";
    auto make_snii = [snii_path]() {
      auto a = std::make_unique<bench::SniiAdapter>();
      a->open_existing(snii_path);
      return a;
    };
    auto make_clu = [clu_path]() {
      auto a = std::make_unique<bench::CluceneAdapter>();
      a->open_existing(clu_path);
      return a;
    };
    // Warm-up single-threaded so process-global lazy inits (CLucene Similarity /
    // FSDirectory cache / string-intern / ThreadLocal) happen outside the timing.
    {
      std::vector<uint32_t> d;
      snii::io::IoMetrics m;
      auto sa = make_snii();
      auto ca = make_clu();
      for (const Scenario& s : scenarios) {
        run_one(*sa, s, &d, &m);
        run_one(*ca, s, &d, &m);
      }
    }
    auto run_q = [](auto& a, const Scenario& s, std::vector<uint32_t>* d,
                    snii::io::IoMetrics* m) { run_one(a, s, d, m); };
    const double secs = args.concurrency_seconds;
    const uint32_t W = args.concurrency;
    std::printf("\n=== per-scenario concurrent QPS (local, N=%u threads, %.0fs each) ===\n",
                W, secs);
    for (const Scenario& s : scenarios) {
      const std::vector<Scenario> one = {s};
      const ConcResult c =
          run_concurrent(make_clu, run_q, one, W, secs, args.concurrency_warmup);
      const ConcResult x =
          run_concurrent(make_snii, run_q, one, W, secs, args.concurrency_warmup);
      std::printf("[%s] CLucene QPS=%.0f p50=%.3f p99=%.3f ms | SNII QPS=%.0f "
                  "p50=%.3f p99=%.3f ms\n",
                  s.id.c_str(), c.qps, c.p50, c.p99, x.qps, x.p50, x.p99);
    }
  }

  std::printf("\nresult: %s (%zu scenarios)\n",
              all_identical ? "ALL DOCIDS IDENTICAL (SNII == CLucene)"
                            : "DOCID MISMATCH",
              scenarios.size());
  return all_identical ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // Return transient build buffers to the OS promptly (glibc retains freed
  // arena memory by default, inflating peak RSS for an allocation-heavy build).
  // mmap large blocks so free() unmaps them; trim the main arena aggressively.
  // Applied to BOTH engines for fairness; production Doris uses jemalloc, which
  // reclaims similarly. Verified CLucene's peak is unchanged by this.
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  mallopt(M_TRIM_THRESHOLD, 128 * 1024);

  const Args args = parse_args(argc, argv);

  // SNII BM25 scoring path-equivalence (exhaustive == wand == wand_selective).
  if (args.scoring) {
    return run_scoring_mode(args);
  }

  // Prefix / MATCH_PHRASE_PREFIX head-to-head (ordered term enumeration).
  if (args.prefix) {
    return run_prefix_mode(args);
  }

  // SNII multi-logical-index write self-consistency (tokenized + keyword mix).
  if (args.multi_index) {
    return run_multi_index_mode(args);
  }

  // Non-tokenized (keyword) docs-only index comparison.
  if (args.keyword) {
    return run_keyword_mode(args);
  }

  // Enriched scenario suite (build both engines + run the df-calibrated catalog).
  if (args.scenarios) {
    return run_scenario_mode(args);
  }

  // Query-only mode: benchmark queries over a persisted index dir (no build).
  if (!args.query_dir.empty()) {
    return run_query_mode(args);
  }

  // Sharded large-scale head-to-head (both engines sharded; query merges).
  if (!args.parquet_file.empty() && args.shard_docs != 0) {
    return run_sharded_e2e(args);
  }

  // E2E real-dataset import mode (parquet source) bypasses the synthetic corpus.
  if (!args.parquet_file.empty()) {
    return run_e2e_mode(args);
  }

  std::printf("=== SNII vs CLucene S3-access benchmark ===\n");
  std::printf("corpus: docs=%u vocab=%u zipf=%.2f doclen=%u seed=%llu\n",
              args.docs, args.vocab, args.zipf, args.doclen,
              static_cast<unsigned long long>(args.seed));

  // 1. Build the corpus: load a REAL one (one doc/line) if --corpus-file is set,
  // else generate the deterministic synthetic corpus.
  const bench::Corpus corpus =
      args.corpus_file.empty()
          ? bench::generate(args.docs, args.vocab, args.zipf, args.doclen, args.seed)
          : bench::load_from_file(args.corpus_file, args.docs);
  if (!args.corpus_file.empty()) {
    std::printf("loaded real corpus: %s docs=%u vocab=%zu\n",
                args.corpus_file.c_str(), corpus.doc_count, corpus.vocab.size());
  }

  // 1a. Resource mode: index size / build CPU / peak RSS (no oracle needed).
  if (args.resources) {
    return run_resources_mode(args, corpus);
  }

  // 1a'. Persist mode: when --out-dir is given for the synthetic corpus, build BOTH
  //      engines onto disk (out_dir/snii/index.idx + out_dir/clucene) and exit. The
  //      pair can then be fed to --query-dir for the local cold/warm latency
  //      distribution (run_query_mode), with no rebuild between samples.
  if (!args.out_dir.empty()) {
    const std::string snii_path = args.out_dir + "/snii/index.idx";
    const std::string clu_dir = args.out_dir + "/clucene";
    try {
      std::filesystem::create_directories(args.out_dir + "/snii");
      std::filesystem::create_directories(clu_dir);
      bench::SniiAdapter snii_idx;
      bench::CluceneAdapter cl_idx;
      snii_idx.build_at(snii_path, corpus, /*keep_on_disk=*/true);
      cl_idx.build_at(clu_dir, corpus, /*keep_on_disk=*/true);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: persist build failed: %s\n", e.what());
      return 1;
    }
    std::printf("persisted synthetic index pair to %s (snii/index.idx + clucene/); "
                "run --query-dir %s for local latency\n",
                args.out_dir.c_str(), args.out_dir.c_str());
    return 0;
  }

  const Oracle oracle(corpus);

  // 1b. Real-OSS mode: build + upload both indexes to OSS and run the SAME
  //     queries reading from OSS, measuring real wall-clock for both engines.
  if (args.oss) {
#ifdef SNII_WITH_S3
    return run_oss_mode(args, corpus, oracle);
#else
    std::fprintf(stderr,
                 "FATAL: --oss requires building with -DSNII_WITH_S3=ON\n");
    return 2;
#endif
  }

  // 2. Build both indexes over the SAME corpus.
  bench::SniiAdapter snii_idx;
  bench::CluceneAdapter cl_idx;
  try {
    snii_idx.build_and_open(corpus);
    cl_idx.build_and_open(corpus);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: index build failed: %s\n", e.what());
    return 1;
  }

  // 3. Pick query terms with a spread of document frequency, plus a real phrase.
  const std::string high_term = corpus.vocab[bench::highest_df_term(corpus)];
  const std::string mid_term = corpus.vocab[bench::mid_df_term(corpus)];
  const std::string low_term = corpus.vocab[bench::low_df_term(corpus)];
  const std::vector<std::string> phrase = bench::extract_phrase(corpus, 5);

  std::printf("query terms: high-df='%s'(df=%zu) mid-df='%s'(df=%zu) "
              "low-df='%s'(df=%zu)\n",
              high_term.c_str(), oracle.term(high_term).size(), mid_term.c_str(),
              oracle.term(mid_term).size(), low_term.c_str(),
              oracle.term(low_term).size());
  std::printf("phrase: '%s'\n", join_words(phrase).c_str());

  // 4. Run the query suite. Each entry runs on SNII, CLucene, and the oracle,
  //    asserting all three docid sets are identical.
  std::vector<QueryResult> results;
  bool all_match = true;

  auto run_term = [&](const std::string& label, const std::string& term) {
    QueryResult r;
    r.label = label + " '" + term + "'";
    std::vector<uint32_t> sdoc, cdoc;
    snii::io::IoMetrics sm, cm;
    try {
      snii_idx.term_query(term, &sdoc, &sm);
      cl_idx.term_query(term, &cdoc, &cm);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: term query '%s' failed: %s\n", term.c_str(),
                   e.what());
      std::exit(1);
    }
    const std::vector<uint32_t> odoc = oracle.term(term);
    r.snii = sm;
    r.clucene = cm;
    r.hits = sdoc.size();
    r.docids_match = (sdoc == cdoc) && (sdoc == odoc);
    if (!r.docids_match) {
      all_match = false;
      std::fprintf(stderr,
                   "MISMATCH on %s: snii=%zu clucene=%zu oracle=%zu\n",
                   r.label.c_str(), sdoc.size(), cdoc.size(), odoc.size());
    }
    results.push_back(r);
  };

  auto run_phrase = [&](const std::vector<std::string>& words) {
    QueryResult r;
    r.label = "PHRASE '" + join_words(words) + "'";
    r.is_phrase = true;
    std::vector<uint32_t> sdoc, cdoc;
    snii::io::IoMetrics sm, cm;
    try {
      snii_idx.phrase_query(words, &sdoc, &sm);
      cl_idx.phrase_query(words, &cdoc, &cm);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FATAL: phrase query failed: %s\n", e.what());
      std::exit(1);
    }
    const std::vector<uint32_t> odoc = oracle.phrase(words);
    r.snii = sm;
    r.clucene = cm;
    r.hits = sdoc.size();
    r.docids_match = (sdoc == cdoc) && (sdoc == odoc);
    if (!r.docids_match) {
      all_match = false;
      std::fprintf(stderr,
                   "MISMATCH on PHRASE: snii=%zu clucene=%zu oracle=%zu\n",
                   sdoc.size(), cdoc.size(), odoc.size());
    }
    results.push_back(r);
  };

  run_term("TERM high-df", high_term);
  run_term("TERM mid-df", mid_term);
  run_term("TERM low-df", low_term);
  if (!phrase.empty()) {
    run_phrase(phrase);
  } else {
    std::fprintf(stderr, "WARNING: no 5-token phrase found in corpus\n");
  }

  // 5. Print the comparison table.
  std::printf("\n=== I/O cost comparison (CLucene vs SNII, same cost model) ===\n");
  for (const QueryResult& r : results) {
    std::printf("[%s]  hits=%zu  docids_match=%s\n", r.label.c_str(), r.hits,
                r.docids_match ? "YES" : "NO");
    print_metrics_row("CLucene", r.clucene);
    print_metrics_row("SNII", r.snii);
    std::printf("  ratio    serial_rounds(CL/SNII)=%.2f  range_gets(CL/SNII)=%.2f\n",
                ratio(r.clucene.serial_rounds, r.snii.serial_rounds),
                ratio(r.clucene.range_gets, r.snii.range_gets));
  }

  std::printf("\nresult: %s\n", all_match ? "ALL DOCIDS MATCH" : "DOCID MISMATCH");
  if (!all_match) return 1;
  return 0;
}
