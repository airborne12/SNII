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
  // Opaque handles; concrete CLucene types live in the .cpp to keep this header
  // free of CLucene includes.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bench
