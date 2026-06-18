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

  // Builds the index at a temporary path and opens it for reading. Throws
  // std::runtime_error on any failure (writer, reader, or open_index).
  void build_and_open(const Corpus& c);

  // Runs term_query for `term`, filling `docids` (ascending) and `metrics` with
  // the I/O accounting for this query alone. Throws on query error.
  void term_query(const std::string& term, std::vector<uint32_t>* docids,
                  snii::io::IoMetrics* metrics);

  // Runs phrase_query for `words`, filling `docids` and `metrics`. Throws on error.
  void phrase_query(const std::vector<std::string>& words,
                    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // On-disk byte size of the built .idx container (0 if not built).
  uint64_t index_bytes() const;

 private:
  std::string path_;
  size_t spill_threshold_bytes_ = 0;  // 0 = unlimited (in-memory build)
  std::unique_ptr<snii::io::LocalFileReader> local_;
  std::unique_ptr<snii::io::MeteredFileReader> metered_;
  std::unique_ptr<snii::reader::SniiSegmentReader> segment_;
  std::unique_ptr<snii::reader::LogicalIndexReader> index_;
};

}  // namespace bench
