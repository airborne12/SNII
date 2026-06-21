#pragma once

// Real-OSS SNII benchmark adapter.
//
// ISOLATION: guarded by SNII_WITH_S3 so the default bench build (no aws) is
// unaffected. When ON, this adapter:
//   1. builds a single-segment .idx from the corpus to a temp local file,
//   2. uploads it to OSS under cfg.prefix as prefix + "/" + key (S3FileWriter),
//   3. opens it over a snii::io::S3FileReader wrapped in a MeteredFileReader, so
//      queries issue REAL ranged OSS GETs accounted by the same cost model used
//      for the CLucene OSS directory.
//
// term_query / phrase_query mirror SniiAdapter, returning ascending docids plus
// per-query I/O metrics (cold cache, reset before each query).
#ifdef SNII_WITH_S3

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "corpus_gen.h"
#include "snii/io/metered_file_reader.h"
#include "snii/io/s3_object_store.h"
#include "snii/reader/logical_index_reader.h"
#include "snii/reader/snii_segment_reader.h"

namespace bench {

class SniiOssAdapter {
 public:
  SniiOssAdapter() = default;
  ~SniiOssAdapter();

  SniiOssAdapter(const SniiOssAdapter&) = delete;
  SniiOssAdapter& operator=(const SniiOssAdapter&) = delete;

  // Keyword (non-tokenized, docs-only) build: set before build_upload_and_open.
  void set_docs_only(bool v) { docs_only_ = v; }

  // Builds the .idx locally, uploads it to OSS under cfg.prefix, and opens it
  // over an S3FileReader. Throws std::runtime_error on failure.
  void build_upload_and_open(const Corpus& c, const snii::io::S3Config& cfg);

  // The OSS object key (prefix + "/" + key) that was uploaded, for cleanup.
  const std::string& uploaded_key() const { return uploaded_key_; }

  // The raw OSS object key (no prefix) passed to S3FileReader::open -- pass it to
  // open_uploaded on a fresh adapter to open the SAME uploaded index per thread.
  const std::string& object_key() const { return object_key_; }

  // Opens an ALREADY-uploaded index (its own S3FileReader + reader chain) without
  // rebuilding/re-uploading -- one per worker thread for concurrent S3 querying.
  void open_uploaded(const std::string& key, const snii::io::S3Config& cfg);

  // term_query for `term`: ascending docids + per-query I/O metrics. Throws on error.
  void term_query(const std::string& term, std::vector<uint32_t>* docids,
                  snii::io::IoMetrics* metrics);

  // phrase_query for `words`: ascending docids + per-query I/O metrics. Throws on error.
  void phrase_query(const std::vector<std::string>& words,
                    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // Boolean / match-all / prefix / match_phrase_prefix -- same surface as the local
  // SniiAdapter, over the OSS reader. Each fills docids + per-query I/O metrics.
  void boolean_and(const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void boolean_or(const std::vector<std::string>& terms,
                  std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void match_all(std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void prefix_query(const std::string& prefix, std::vector<uint32_t>* docids,
                    snii::io::IoMetrics* metrics);
  std::vector<std::string> enumerate_prefix(const std::string& prefix);
  void phrase_prefix_query(const std::vector<std::string>& fixed,
                           const std::vector<std::string>& expansions,
                           std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

 private:
  bool docs_only_ = false;
  std::string local_path_;   // temp local .idx (removed in dtor)
  std::string uploaded_key_;  // full OSS key (prefix + "/" + key)
  std::string object_key_;    // raw key for S3FileReader::open / open_uploaded
  std::unique_ptr<snii::io::S3FileReader> s3_;
  std::unique_ptr<snii::io::MeteredFileReader> metered_;
  std::unique_ptr<snii::reader::SniiSegmentReader> segment_;
  std::unique_ptr<snii::reader::LogicalIndexReader> index_;
};

}  // namespace bench

#endif  // SNII_WITH_S3
