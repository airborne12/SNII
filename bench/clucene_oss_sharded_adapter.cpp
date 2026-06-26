#include "clucene_oss_sharded_adapter.h"

#ifdef SNII_WITH_S3

#include <algorithm>
#include <stdexcept>

namespace bench {
namespace {

void add_metrics(snii::io::IoMetrics* dst, const snii::io::IoMetrics& src) {
  dst->read_at_calls += src.read_at_calls;
  dst->serial_rounds += src.serial_rounds;
  dst->range_gets += src.range_gets;
  dst->remote_bytes += src.remote_bytes;
  dst->total_request_bytes += src.total_request_bytes;
}

void require_single_shard(size_t n) {
  if (n != 1) {
    throw std::runtime_error("CLucene OSS sharded adapter: expected single shard");
  }
}

void term_fn(CluceneOssAdapter& a, std::vector<uint32_t>* d,
             snii::io::IoMetrics* m, const void* arg) {
  a.term_query(*static_cast<const std::string*>(arg), d, m);
}

void phrase_fn(CluceneOssAdapter& a, std::vector<uint32_t>* d,
               snii::io::IoMetrics* m, const void* arg) {
  a.phrase_query(*static_cast<const std::vector<std::string>*>(arg), d, m);
}

void and_fn(CluceneOssAdapter& a, std::vector<uint32_t>* d,
            snii::io::IoMetrics* m, const void* arg) {
  a.boolean_and(*static_cast<const std::vector<std::string>*>(arg), d, m);
}

void or_fn(CluceneOssAdapter& a, std::vector<uint32_t>* d,
           snii::io::IoMetrics* m, const void* arg) {
  a.boolean_or(*static_cast<const std::vector<std::string>*>(arg), d, m);
}

void match_all_fn(CluceneOssAdapter& a, std::vector<uint32_t>* d,
                  snii::io::IoMetrics* m, const void* /*arg*/) {
  a.match_all(d, m);
}

void prefix_fn(CluceneOssAdapter& a, std::vector<uint32_t>* d,
               snii::io::IoMetrics* m, const void* arg) {
  a.prefix_query(*static_cast<const std::string*>(arg), d, m);
}

struct PhrasePrefixArgs {
  const std::vector<std::string>* fixed = nullptr;
  const std::vector<std::string>* expansions = nullptr;
};

void phrase_prefix_fn(CluceneOssAdapter& a, std::vector<uint32_t>* d,
                      snii::io::IoMetrics* m, const void* arg) {
  const auto* p = static_cast<const PhrasePrefixArgs*>(arg);
  a.phrase_prefix_query(*p->fixed, *p->expansions, d, m);
}

}  // namespace

void CluceneOssShardedAdapter::build_upload_and_open(
    const Corpus& c, const snii::io::S3Config& cfg, uint32_t shard_docs) {
  if (c.doc_count == 0) {
    throw std::runtime_error("CLucene OSS sharded adapter: empty corpus");
  }
  uploaded_keys_.clear();
  shards_.clear();
  index_bytes_ = 0;

  const uint32_t docs_per_shard = shard_docs == 0 ? c.doc_count : shard_docs;
  for (uint32_t lo = 0; lo < c.doc_count; lo += docs_per_shard) {
    const uint32_t hi = std::min(lo + docs_per_shard, c.doc_count);
    auto adapter = std::make_unique<CluceneOssAdapter>();
    adapter->set_docs_only(docs_only_);
    adapter->set_no_merge(no_merge_);
    adapter->build_upload_and_open_range(c, lo, hi, cfg);
    index_bytes_ += adapter->index_bytes();
    const std::vector<std::string>& keys = adapter->uploaded_keys();
    uploaded_keys_.insert(uploaded_keys_.end(), keys.begin(), keys.end());
    shards_.push_back(Shard{lo, std::move(adapter)});
  }
}

const std::string& CluceneOssShardedAdapter::object_prefix() const {
  require_single_shard(shards_.size());
  return shards_.front().adapter->object_prefix();
}

const std::vector<std::string>& CluceneOssShardedAdapter::file_names() const {
  require_single_shard(shards_.size());
  return shards_.front().adapter->file_names();
}

void CluceneOssShardedAdapter::run_all(QueryFn fn, const void* arg,
                                       std::vector<uint32_t>* docids,
                                       snii::io::IoMetrics* metrics) {
  docids->clear();
  *metrics = snii::io::IoMetrics{};
  std::vector<uint32_t> local;
  for (Shard& shard : shards_) {
    local.clear();
    snii::io::IoMetrics m;
    fn(*shard.adapter, &local, &m, arg);
    docids->reserve(docids->size() + local.size());
    for (uint32_t d : local) docids->push_back(shard.base + d);
    add_metrics(metrics, m);
  }
}

void CluceneOssShardedAdapter::term_query(const std::string& term,
                                          std::vector<uint32_t>* docids,
                                          snii::io::IoMetrics* metrics) {
  run_all(term_fn, &term, docids, metrics);
}

void CluceneOssShardedAdapter::phrase_query(
    const std::vector<std::string>& words, std::vector<uint32_t>* docids,
    snii::io::IoMetrics* metrics) {
  run_all(phrase_fn, &words, docids, metrics);
}

void CluceneOssShardedAdapter::boolean_and(
    const std::vector<std::string>& terms, std::vector<uint32_t>* docids,
    snii::io::IoMetrics* metrics) {
  run_all(and_fn, &terms, docids, metrics);
}

void CluceneOssShardedAdapter::boolean_or(
    const std::vector<std::string>& terms, std::vector<uint32_t>* docids,
    snii::io::IoMetrics* metrics) {
  run_all(or_fn, &terms, docids, metrics);
}

void CluceneOssShardedAdapter::match_all(std::vector<uint32_t>* docids,
                                         snii::io::IoMetrics* metrics) {
  run_all(match_all_fn, nullptr, docids, metrics);
}

void CluceneOssShardedAdapter::prefix_query(const std::string& prefix,
                                            std::vector<uint32_t>* docids,
                                            snii::io::IoMetrics* metrics) {
  run_all(prefix_fn, &prefix, docids, metrics);
}

void CluceneOssShardedAdapter::phrase_prefix_query(
    const std::vector<std::string>& fixed,
    const std::vector<std::string>& expansions, std::vector<uint32_t>* docids,
    snii::io::IoMetrics* metrics) {
  const PhrasePrefixArgs args{&fixed, &expansions};
  run_all(phrase_prefix_fn, &args, docids, metrics);
}

}  // namespace bench

#endif  // SNII_WITH_S3
