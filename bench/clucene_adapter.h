#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "corpus_gen.h"
#include "snii/io/metered_file_reader.h"

// CLucene benchmark adapter: builds a compound (.cfs) index from the SAME corpus
// as the SNII adapter, with documents inserted in corpus doc-id order and no
// deletions (so CLucene docid == corpus docid == SNII docid). Every physical
// read of the index files is routed through a snii::io::MeteredFileReader, so
// CLucene's cursor reads are measured by the exact same object-storage cost
// model as the SNII reader.
namespace bench {

class MeteredDirectory;  // implementation detail (see clucene_adapter.cpp)

class CluceneAdapter {
 public:
  CluceneAdapter();
  ~CluceneAdapter();

  CluceneAdapter(const CluceneAdapter&) = delete;
  CluceneAdapter& operator=(const CluceneAdapter&) = delete;

  // Sets the IndexWriter RAM-buffer flush threshold (MiB) to MATCH the SNII
  // spill threshold for a fair build-memory comparison. <=0 means no auto flush
  // (the whole index is built in RAM, matching SNII --spill-mib 0). When >0,
  // CLucene flushes a segment every `mb` MiB (doc-count auto-flush disabled),
  // matching SNII's spill-at-`mb` behavior.
  void set_ram_buffer_mb(double mb) { ram_buffer_mb_ = mb; }

  // When true, the writer keeps periodic segment flushes but SKIPS optimize()
  // (no segment merge). This both (a) bounds each in-RAM segment so its posting
  // pool never overflows the 32-bit position offset, and (b) avoids the merge
  // path entirely -- the two ways a single huge segment crashes this CLucene
  // fork past ~2^30 positions. The resulting multi-segment index is read
  // natively by IndexReader (a multi-segment reader); docids stay in corpus
  // insertion order across segments. Required to index corpora whose total
  // positions exceed ~2^30.
  void set_no_merge(bool v) { no_merge_ = v; }

  // Build a NON-TOKENIZED (keyword) index: docs-only via omitTermFreqAndPositions
  // (no freq/positions, smaller index, exact-match only). Default false. Pair with
  // a single-token keyword corpus so each value is one term. Set before build_at.
  void set_docs_only(bool v) { docs_only_ = v; }

  // Builds the compound index in a temp directory and opens a searcher over a
  // metered directory. Throws std::runtime_error on failure.
  void build_and_open(const Corpus& c);

  // Builds the index into the EXACT directory `dir` (created if absent) and opens
  // a searcher over it. When `keep_on_disk` is true the directory is NOT removed
  // on destruction, so the E2E harness can leave the real segment files behind.
  // Throws std::runtime_error on failure.
  void build_at(const std::string& dir, const Corpus& c, bool keep_on_disk);

  // Builds a CLucene index for the corpus doc range [doc_lo, doc_hi) into `dir`,
  // with LOCAL docids 0..(doc_hi-doc_lo-1) (CLucene assigns them in add order).
  // Used to build a multi-shard CLucene index (one dir per shard) so each shard's
  // total positions stay under this fork's ~2^30 limit; queries open every shard
  // and merge with a base-docid offset. Throws std::runtime_error on failure.
  void build_range(const std::string& dir, const Corpus& c, uint32_t doc_lo,
                   uint32_t doc_hi, bool keep_on_disk);

  // The on-disk segment files backing this index, for `ls`-style reporting.
  // Empty if not built.
  std::vector<std::string> index_files() const;

  // Opens an ALREADY-built index directory `dir` for reading only (no build),
  // through the same metered directory used after a build. The directory is left
  // on disk on destruction. Throws std::runtime_error on failure.
  void open_existing(const std::string& dir);

  // Runs a TermQuery on field "body" and returns ascending docids plus the
  // aggregated I/O metrics for this query alone (the metered directory is reset
  // first). Throws on failure.
  void term_query(const std::string& term, std::vector<uint32_t>* docids,
                  snii::io::IoMetrics* metrics);

  // Runs a PhraseQuery (one Term per word) on field "body" and returns ascending
  // docids plus aggregated I/O metrics for this query alone. Throws on failure.
  void phrase_query(const std::vector<std::string>& words,
                    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // Boolean AND (BooleanQuery, MUST clauses), boolean OR (SHOULD clauses), and
  // match-all (every docid; no query). Each fills ascending docids + aggregated
  // I/O metrics for that query alone. Throw on failure.
  void boolean_and(const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void boolean_or(const std::vector<std::string>& terms,
                  std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void match_all(std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // Total on-disk byte size of all index segment files (0 if not built).
  uint64_t index_bytes() const;

 private:
  // Doc count CLucene should buffer before flushing a segment so its per-flush
  // RAM matches SNII's spill-at-`ram_buffer_mb_` MiB (lever 4 fairness). Derived
  // from the corpus's average doc length and the same per-doc byte model SNII uses.
  int32_t flush_doc_count(const Corpus& c) const;

  // Opens impl_->directory + IndexReader + searcher over impl_->dir_path (shared
  // by build_at's read phase and open_existing).
  void open_metered_reader();

  // Shared BooleanQuery runner: MUST clauses (conjunction) or SHOULD (disjunction).
  void boolean_query(const std::vector<std::string>& terms, bool conjunction,
                     std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // RAM-buffer flush threshold (MiB); <=0 = no auto flush (build fully in RAM).
  double ram_buffer_mb_ = 0.0;
  // When true: periodic flush + skip optimize() (multi-segment, no merge).
  bool no_merge_ = false;
  bool docs_only_ = false;  // true = keyword (docs-only, omit freq/positions)
  // Opaque handles; concrete CLucene types live in the .cpp to keep this header
  // free of CLucene includes.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bench
