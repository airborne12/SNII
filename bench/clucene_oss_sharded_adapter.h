#pragma once

#ifdef SNII_WITH_S3

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clucene_oss_adapter.h"
#include "corpus_gen.h"
#include "snii/io/metered_file_reader.h"
#include "snii/io/s3_object_store.h"

namespace bench {

// CLucene OSS 分片包装器。每个 shard 是一个独立 CLucene 索引目录，负责
// corpus 的连续 doc range；查询时把 shard-local docid 加回 base docid。
class CluceneOssShardedAdapter {
 public:
  CluceneOssShardedAdapter() = default;
  ~CluceneOssShardedAdapter() = default;

  CluceneOssShardedAdapter(const CluceneOssShardedAdapter&) = delete;
  CluceneOssShardedAdapter& operator=(const CluceneOssShardedAdapter&) = delete;

  void set_docs_only(bool v) { docs_only_ = v; }
  void set_no_merge(bool v) { no_merge_ = v; }

  void build_upload_and_open(const Corpus& c, const snii::io::S3Config& cfg,
                             uint32_t shard_docs);

  const std::vector<std::string>& uploaded_keys() const { return uploaded_keys_; }
  uint64_t index_bytes() const { return index_bytes_; }
  size_t shard_count() const { return shards_.size(); }

  // 只在 single-shard 模式下供并发 benchmark 复用已有 run_oss_concurrent。
  const std::string& object_prefix() const;
  const std::vector<std::string>& file_names() const;

  void term_query(const std::string& term, std::vector<uint32_t>* docids,
                  snii::io::IoMetrics* metrics);
  void phrase_query(const std::vector<std::string>& words,
                    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void boolean_and(const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void boolean_or(const std::vector<std::string>& terms,
                  std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void match_all(std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics);
  void prefix_query(const std::string& prefix, std::vector<uint32_t>* docids,
                    snii::io::IoMetrics* metrics);
  void phrase_prefix_query(const std::vector<std::string>& fixed,
                           const std::vector<std::string>& expansions,
                           std::vector<uint32_t>* docids,
                           snii::io::IoMetrics* metrics);

 private:
  struct Shard {
    uint32_t base = 0;
    std::unique_ptr<CluceneOssAdapter> adapter;
  };

  using QueryFn = void (*)(CluceneOssAdapter&, std::vector<uint32_t>*,
                           snii::io::IoMetrics*, const void*);

  void run_all(QueryFn fn, const void* arg, std::vector<uint32_t>* docids,
               snii::io::IoMetrics* metrics);

  bool docs_only_ = false;
  bool no_merge_ = false;
  uint64_t index_bytes_ = 0;
  std::vector<std::string> uploaded_keys_;
  std::vector<Shard> shards_;
};

}  // namespace bench

#endif  // SNII_WITH_S3
