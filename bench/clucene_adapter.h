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

  // Builds the compound index in a temp directory and opens a searcher over a
  // metered directory. Throws std::runtime_error on failure.
  void build_and_open(const Corpus& c);

  // Runs a TermQuery on field "body" and returns ascending docids plus the
  // aggregated I/O metrics for this query alone (the metered directory is reset
  // first). Throws on failure.
  void term_query(const std::string& term, std::vector<uint32_t>* docids,
                  snii::io::IoMetrics* metrics);

  // Runs a PhraseQuery (one Term per word) on field "body" and returns ascending
  // docids plus aggregated I/O metrics for this query alone. Throws on failure.
  void phrase_query(const std::vector<std::string>& words,
                    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // Total on-disk byte size of all index segment files (0 if not built).
  uint64_t index_bytes() const;

 private:
  // Doc count CLucene should buffer before flushing a segment so its per-flush
  // RAM matches SNII's spill-at-`ram_buffer_mb_` MiB (lever 4 fairness). Derived
  // from the corpus's average doc length and the same per-doc byte model SNII uses.
  int32_t flush_doc_count(const Corpus& c) const;

  // RAM-buffer flush threshold (MiB); <=0 = no auto flush (build fully in RAM).
  double ram_buffer_mb_ = 0.0;
  // Opaque handles; concrete CLucene types live in the .cpp to keep this header
  // free of CLucene includes.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bench
