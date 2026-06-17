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

#include <algorithm>
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

namespace {

struct Args {
  uint32_t docs = 2000;
  uint32_t vocab = 2000;
  double zipf = 1.1;
  uint32_t doclen = 12;
  uint64_t seed = 42;
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
  std::printf("  %-8s read_at=%-6llu serial_rounds=%-4llu range_gets=%-4llu "
              "remote_bytes=%-9llu\n",
              engine,
              static_cast<unsigned long long>(m.read_at_calls),
              static_cast<unsigned long long>(m.serial_rounds),
              static_cast<unsigned long long>(m.range_gets),
              static_cast<unsigned long long>(m.remote_bytes));
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parse_args(argc, argv);

  std::printf("=== SNII vs CLucene S3-access benchmark ===\n");
  std::printf("corpus: docs=%u vocab=%u zipf=%.2f doclen=%u seed=%llu\n",
              args.docs, args.vocab, args.zipf, args.doclen,
              static_cast<unsigned long long>(args.seed));

  // 1. Generate the deterministic corpus and the oracle.
  const bench::Corpus corpus =
      bench::generate(args.docs, args.vocab, args.zipf, args.doclen, args.seed);
  const Oracle oracle(corpus);

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
