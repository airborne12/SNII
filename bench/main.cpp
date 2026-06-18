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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "clucene_adapter.h"
#include "corpus_gen.h"
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

// Builds one engine over the corpus, reporting its on-disk index size and the
// CPU it took. Returns nothing; peak RSS is read at process exit by the caller.
template <typename Adapter>
void build_and_report(const char* name, const bench::Corpus& corpus,
                      uint32_t spill_mib) {
  Adapter idx;
  apply_spill(idx, spill_mib);
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
    if (snii) build_and_report<bench::SniiAdapter>("SNII", corpus, args.spill_mib);
    // Same threshold for CLucene: its RAM-buffer flush matches SNII's spill.
    if (clu) build_and_report<bench::CluceneAdapter>("CLucene", corpus, args.spill_mib);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: resource build failed: %s\n", e.what());
    return 1;
  }
  std::printf("  peak_rss=%.1f MiB (process high-water; engine=%s)\n",
              peak_rss_mib(), args.engine.c_str());
  return 0;
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

  std::printf("=== SNII vs CLucene S3-access benchmark ===\n");
  std::printf("corpus: docs=%u vocab=%u zipf=%.2f doclen=%u seed=%llu\n",
              args.docs, args.vocab, args.zipf, args.doclen,
              static_cast<unsigned long long>(args.seed));

  // 1. Generate the deterministic corpus.
  const bench::Corpus corpus =
      bench::generate(args.docs, args.vocab, args.zipf, args.doclen, args.seed);

  // 1a. Resource mode: index size / build CPU / peak RSS (no oracle needed).
  if (args.resources) {
    return run_resources_mode(args, corpus);
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
