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

  // Keyword (non-tokenized, docs-only via omitTermFreqAndPositions) build: set
  // before build_upload_and_open.
  void set_docs_only(bool v) { docs_only_ = v; }

  // CLucene 大规模构建时保留默认小段 flush，但跳过 optimize()，避免合成超大段。
  void set_no_merge(bool v) { no_merge_ = v; }

  // Builds the index locally, uploads it to OSS under cfg.prefix, and opens a
  // searcher over the OSS directory. Throws std::runtime_error on failure.
  void build_upload_and_open(const Corpus& c, const snii::io::S3Config& cfg);

  // 只构建 corpus 的 [doc_lo, doc_hi) 范围，CLucene 本地 docid 从 0 开始。
  // 调用方负责在分片查询时加回 base docid。
  void build_upload_and_open_range(const Corpus& c, uint32_t doc_lo,
                                   uint32_t doc_hi,
                                   const snii::io::S3Config& cfg);

  // The OSS object keys (prefix + "/" + name) that were uploaded, for best-effort
  // cleanup by the caller.
  const std::vector<std::string>& uploaded_keys() const;
  uint64_t index_bytes() const;

  // The CLucene segment file names of the uploaded index -- pass them to
  // open_uploaded on a fresh adapter to open the SAME index per worker thread.
  const std::vector<std::string>& file_names() const;

  // 当前 adapter 的远端 OSS prefix，file_names() 中的文件都在该目录下。
  const std::string& object_prefix() const;

  // Opens an ALREADY-uploaded index (its own OSS directory + reader + searcher)
  // without rebuilding/re-uploading -- one per worker thread for concurrent S3.
  void open_uploaded(const snii::io::S3Config& cfg,
                     const std::vector<std::string>& file_names);

  // TermQuery on field "body": ascending docids + per-query I/O metrics (the OSS
  // directory's metered readers are reset first). Throws on failure.
  void term_query(const std::string& term, std::vector<uint32_t>* docids,
                  snii::io::IoMetrics* metrics);

  // PhraseQuery (one Term per word) on field "body": ascending docids + per-query
  // I/O metrics. Throws on failure.
  void phrase_query(const std::vector<std::string>& words,
                    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

  // Boolean / match-all / prefix / match_phrase_prefix -- same surface as the local
  // CluceneAdapter, over the OSS directory. Each fills docids + per-query metrics.
  void boolean_and(const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void boolean_or(const std::vector<std::string>& terms,
                  std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void match_all(std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void prefix_query(const std::string& prefix, std::vector<uint32_t>* docids,
                    snii::io::IoMetrics* metrics);
  void phrase_prefix_query(const std::vector<std::string>& fixed,
                           const std::vector<std::string>& expansions,
                           std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);

 private:
  void boolean_query(const std::vector<std::string>& terms, bool conjunction,
                     std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  bool docs_only_ = false;
  bool no_merge_ = false;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bench

#endif  // SNII_WITH_S3
