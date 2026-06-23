#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "corpus_gen.h"
#include "snii/io/local_file.h"
#include "snii/io/metered_file_reader.h"
#include "snii/reader/logical_index_reader.h"
#include "snii/reader/snii_segment_reader.h"
#include "snii/stats/snii_stats_provider.h"

// SNII benchmark adapter: builds a single-segment .idx from a Corpus and exposes
// metered term / phrase queries. Each query resets the metered reader first so
// the returned IoMetrics describe that query in isolation against a cold cache.
namespace bench {

class SniiAdapter {
 public:
  SniiAdapter() = default;
  ~SniiAdapter();

  SniiAdapter(const SniiAdapter&) = delete;
  SniiAdapter& operator=(const SniiAdapter&) = delete;

  // Sets the SPIMI spill threshold in bytes (0 = unlimited / pure in-memory).
  // When non-zero, the build bounds input RAM by spilling sorted runs to disk
  // and k-way-merging them, so peak RSS stops scaling with total postings.
  void set_spill_threshold_bytes(size_t bytes) { spill_threshold_bytes_ = bytes; }

  // Build a NON-TOKENIZED (keyword) index: docs-only (IndexConfig::kDocsOnly, no
  // positions/freqs) for exact-match/range on whole-value terms. Default false
  // (tokenized docs+positions). Set before build_at/build_range.
  void set_docs_only(bool v) { docs_only_ = v; }

  // Build a SCORING index (IndexConfig::kDocsPositionsScoring: docs+freq+positions
  // + per-doc norms + stats) for BM25 top-K. Default false. Set before build_at.
  void set_scoring(bool v) { scoring_ = v; }

  // BM25 top-K retrieval path: exhaustive baseline, WAND block-max pruning, or
  // selective-fetch WAND (reads only surviving windows). All three MUST return the
  // same top-K (the design invariant); they differ only in I/O / scoring work.
  enum class ScorePath { kExhaustive, kWand, kWandSelective };

  // One scored hit (docid + BM25 score).
  struct ScoredHit {
    uint32_t docid;
    double score;
  };

  // BM25 top-`k` over `terms` via `path`, filling `out` (score-desc, docid-tiebreak)
  // and `metrics` (this query's I/O alone). Requires a scoring index. Throws on error.
  void score_query(const std::vector<std::string>& terms, uint32_t k, ScorePath path,
                   std::vector<ScoredHit>* out, snii::io::IoMetrics* metrics);

  // One logical index in a multi-index container: its own corpus (vocab + docs),
  // a unique suffix, and whether it is keyword (docs-only) or tokenized.
  struct LogicalSpec {
    const Corpus* corpus;
    std::string suffix;
    bool docs_only;
  };

  // Builds ONE container at `path` holding multiple logical indexes (index_id =
  // 1..N in spec order), e.g. several tokenized fields or a tokenized + keyword
  // mix. Opens index_id 1 for reading; use open_logical to query another. Throws
  // std::runtime_error on failure.
  void build_multi(const std::string& path, const std::vector<LogicalSpec>& specs,
                   bool keep_on_disk);

  // Re-points the active reader to logical index (index_id, suffix) in the open
  // container so term_query/phrase_query/etc. address that field. Throws on error.
  void open_logical(uint32_t index_id, const std::string& suffix);

  // Builds the index at a temporary path and opens it for reading. Throws
  // std::runtime_error on any failure (writer, reader, or open_index).
  void build_and_open(const Corpus& c);

  // Builds the index at the EXACT path `path` (creating parent directories) and
  // opens it for reading. When `keep_on_disk` is true the file is NOT removed on
  // destruction, so the E2E harness can leave a real, inspectable .idx behind.
  // Throws std::runtime_error on any failure.
  void build_at(const std::string& path, const Corpus& c, bool keep_on_disk);

  // Builds a SINGLE SEGMENT from the corpus doc range [doc_lo, doc_hi) at `path`,
  // with LOCAL docids 0..(doc_hi-doc_lo-1). The caller maps a result docid back to
  // the global space by adding doc_lo. Used to build a multi-segment SNII index
  // (one .idx per shard) so total positions per segment stay bounded; queries open
  // every segment and merge. Throws std::runtime_error on failure.
  void build_range(const std::string& path, const Corpus& c, uint32_t doc_lo,
                   uint32_t doc_hi, bool keep_on_disk);

  // The on-disk files backing this index (one .idx container), for `ls`-style
  // reporting. Empty if not built.
  std::vector<std::string> index_files() const;

  // Opens an ALREADY-built .idx at `path` for reading only (no build), proving a
  // persisted container can be reopened by a fresh reader. The file is left on
  // disk on destruction (this adapter did not create it). Throws on failure.
  void open_existing(const std::string& path);

  // Runs term_query for `term`, filling `docids` (ascending) and `metrics` with
  // the I/O accounting for this query alone. Throws on query error.
  void term_query(const std::string& term, std::vector<uint32_t>* docids,
                  snii::io::IoMetrics* metrics);

  // Runs phrase_query for `words`, filling `docids` and `metrics`. Throws on error.
  void phrase_query(const std::vector<std::string>& words,
                    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // Boolean AND (docs containing every term; docid-only conjunction, no positions),
  // boolean OR (union), and match-all (every docid; no postings I/O). Each fills
  // ascending docids + the I/O metrics for that query alone. Throw on error.
  void boolean_and(const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void boolean_or(const std::vector<std::string>& terms,
                  std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void match_all(std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // Prefix query (every term sharing `prefix`, union of their docids) via the
  // ordered term enumeration. enumerate_prefix returns just the matching terms (for
  // building a MATCH_PHRASE_PREFIX). phrase_prefix_query unions phrase(fixed +
  // expansion) over every expansion -- docs where `fixed` occurs consecutively
  // followed by ANY term in `expansions`. Each fills docids + metrics. Throw on error.
  void prefix_query(const std::string& prefix, std::vector<uint32_t>* docids,
                    snii::io::IoMetrics* metrics);
  std::vector<std::string> enumerate_prefix(const std::string& prefix);
  void phrase_prefix_query(const std::vector<std::string>& fixed,
                           const std::vector<std::string>& expansions,
                           std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // On-disk byte size of the built .idx container (0 if not built).
  uint64_t index_bytes() const;

 private:
  // Opens path_ through a fresh metered local reader chain (shared by build_at
  // and open_existing).
  void open_reader();

  std::string path_;
  bool keep_path_ = false;  // when true, the destructor leaves path_ on disk
  size_t spill_threshold_bytes_ = 0;  // 0 = unlimited (in-memory build)
  bool docs_only_ = false;            // true = keyword (docs-only, no positions)
  bool scoring_ = false;              // true = scoring index (norms + stats)
  std::unique_ptr<snii::stats::SniiStatsProvider> stats_;  // resident, for scoring
  std::unique_ptr<snii::io::LocalFileReader> local_;
  std::unique_ptr<snii::io::MeteredFileReader> metered_;
  std::unique_ptr<snii::reader::SniiSegmentReader> segment_;
  std::unique_ptr<snii::reader::LogicalIndexReader> index_;
};

}  // namespace bench
