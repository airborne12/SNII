#pragma once

// Real-OSS CLucene benchmark adapter.
//
// ISOLATION: guarded by SNII_WITH_S3 so the default bench build (no aws) is
// unaffected. When ON, this adapter:
//   1. builds the same compound CLucene index as CluceneAdapter into a temp
//      FSDirectory (documents in corpus doc-id order, no deletions),
//   2. enumerates the generated segment files and uploads each to OSS under
//      cfg.prefix as prefix + "/" + name (via snii::io::S3FileWriter),
//   3. opens an IndexReader over an OssCluceneDirectory so that every physical
//      read during search is a real ranged OSS GET accounted by the same
//      snii::io::MeteredFileReader cost model as the SNII reader.
//
// term_query / phrase_query mirror CluceneAdapter exactly, returning ascending
// docids plus the I/O metrics for that query alone (cold cache).
#ifdef SNII_WITH_S3

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "corpus_gen.h"
#include "snii/io/metered_file_reader.h"
#include "snii/io/s3_object_store.h"

namespace bench {

class CluceneOssAdapter {
 public:
  CluceneOssAdapter();
  ~CluceneOssAdapter();

  CluceneOssAdapter(const CluceneOssAdapter&) = delete;
  CluceneOssAdapter& operator=(const CluceneOssAdapter&) = delete;

  // Builds the index locally, uploads it to OSS under cfg.prefix, and opens a
  // searcher over the OSS directory. Throws std::runtime_error on failure.
  void build_upload_and_open(const Corpus& c, const snii::io::S3Config& cfg);

  // The OSS object keys (prefix + "/" + name) that were uploaded, for best-effort
  // cleanup by the caller.
  const std::vector<std::string>& uploaded_keys() const;

  // TermQuery on field "body": ascending docids + per-query I/O metrics (the OSS
  // directory's metered readers are reset first). Throws on failure.
  void term_query(const std::string& term, std::vector<uint32_t>* docids,
                  snii::io::IoMetrics* metrics);

  // PhraseQuery (one Term per word) on field "body": ascending docids + per-query
  // I/O metrics. Throws on failure.
  void phrase_query(const std::vector<std::string>& words,
                    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bench

#endif  // SNII_WITH_S3
