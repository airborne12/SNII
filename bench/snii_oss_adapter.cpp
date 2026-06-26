#include "snii_oss_adapter.h"

#ifdef SNII_WITH_S3

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

#include "docid_range.h"
#include "snii/common/slice.h"
#include "snii/format/format_constants.h"
#include "snii/io/local_file.h"
#include "snii/query/boolean_query.h"
#include "snii/query/phrase_query.h"
#include "snii/query/prefix_query.h"
#include "snii/query/term_query.h"
#include "snii/writer/snii_compound_writer.h"
#include "snii/writer/spimi_term_buffer.h"

namespace bench {
namespace {

std::string make_temp_path() {
  static int counter = 0;
  return "/tmp/snii_bench_oss_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter++) + ".idx";
}

std::string make_object_key() {
  static int counter = 0;
  return "snii_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter++) + ".idx";
}

[[noreturn]] void fail(const std::string& what, const snii::Status& s) {
  throw std::runtime_error("SNII OSS adapter: " + what + ": " + s.to_string());
}

snii::Status read_whole_file(const std::string& path,
                             std::vector<uint8_t>* out) {
  snii::io::LocalFileReader r;
  if (snii::Status s = r.open(path); !s.ok()) return s;
  return r.read_at(0, r.size(), out);
}

}  // namespace

SniiOssAdapter::~SniiOssAdapter() {
  if (!local_path_.empty()) std::remove(local_path_.c_str());
}

void SniiOssAdapter::build_upload_and_open(const Corpus& c,
                                           const snii::io::S3Config& cfg) {
  using namespace snii;
  using snii::format::IndexConfig;
  using snii::writer::SniiCompoundWriter;
  using snii::writer::SniiIndexInput;
  using snii::writer::SpimiTermBuffer;
  using snii::writer::TermPostings;

  // 1. Accumulate every (term-id, doc, position) into the SPIMI buffer. Corpus
  // tokens are dense ids into c.vocab; feed the raw id (buf borrows c.vocab) so
  // there is no per-token string work.
  SpimiTermBuffer buf(&c.vocab, /*has_positions=*/!docs_only_);
  for (uint32_t d = 0; d < c.doc_count; ++d) {
    const auto& toks = c.docs[d];
    for (uint32_t pos = 0; pos < toks.size(); ++pos) {
      buf.add_token(toks[pos], d, pos);
    }
  }
  std::vector<TermPostings> terms = buf.finalize_sorted();

  // 2. Describe the single logical index (keyword: docs-only; else docs+positions).
  SniiIndexInput in;
  in.index_id = 1;
  in.index_suffix = "body";
  in.config = docs_only_ ? IndexConfig::kDocsOnly : IndexConfig::kDocsPositions;
  in.doc_count = c.doc_count;
  in.terms = std::move(terms);
  in.target_dict_block_bytes = 512;

  // 3. Write the compound container to a temp local file.
  local_path_ = make_temp_path();
  io::LocalFileWriter writer;
  if (Status s = writer.open(local_path_); !s.ok()) fail("open writer", s);
  SniiCompoundWriter compound(&writer);
  if (Status s = compound.add_logical_index(in); !s.ok()) fail("add index", s);
  if (Status s = compound.finish(); !s.ok()) fail("finish writer", s);

  // 4. Upload the .idx to OSS.
  std::vector<uint8_t> idx_bytes;
  if (Status s = read_whole_file(local_path_, &idx_bytes); !s.ok()) {
    fail("read local .idx", s);
  }
  index_bytes_ = idx_bytes.size();
  const std::string key = make_object_key();
  {
    io::S3FileWriter w;
    if (Status s = w.open(cfg, key); !s.ok()) fail("S3 open", s);
    if (Status s = w.append(Slice(idx_bytes)); !s.ok()) fail("S3 append", s);
    if (Status s = w.finalize(); !s.ok()) fail("S3 finalize", s);
  }
  object_key_ = key;
  uploaded_key_ = cfg.prefix + "/" + key;

  // 5. Open over an S3FileReader wrapped in a metered cost model.
  open_uploaded(key, cfg);
}

void SniiOssAdapter::open_uploaded(const std::string& key,
                                   const snii::io::S3Config& cfg) {
  using namespace snii;
  object_key_ = key;
  s3_ = std::make_unique<io::S3FileReader>();
  if (Status s = io::S3FileReader::open(cfg, key, s3_.get()); !s.ok()) {
    fail("S3 read open", s);
  }
  metered_ = std::make_unique<io::MeteredFileReader>(s3_.get());

  segment_ = std::make_unique<reader::SniiSegmentReader>();
  if (Status s = reader::SniiSegmentReader::open(metered_.get(), segment_.get());
      !s.ok()) {
    fail("open segment", s);
  }
  index_ = std::make_unique<reader::LogicalIndexReader>();
  if (Status s = segment_->open_index(1, "body", index_.get()); !s.ok()) {
    fail("open logical index", s);
  }
}

void SniiOssAdapter::term_query(const std::string& term,
                                std::vector<uint32_t>* docids,
                                snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  if (snii::Status s = snii::query::term_query(*index_, term, docids); !s.ok()) {
    fail("term_query(" + term + ")", s);
  }
  *metrics = metered_->metrics();
}

void SniiOssAdapter::phrase_query(const std::vector<std::string>& words,
                                  std::vector<uint32_t>* docids,
                                  snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  if (snii::Status s = snii::query::phrase_query(*index_, words, docids);
      !s.ok()) {
    fail("phrase_query", s);
  }
  *metrics = metered_->metrics();
}

void SniiOssAdapter::boolean_and(const std::vector<std::string>& terms,
                                 std::vector<uint32_t>* docids,
                                 snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  if (snii::Status s = snii::query::boolean_and(*index_, terms, docids); !s.ok()) {
    fail("boolean_and", s);
  }
  *metrics = metered_->metrics();
}

void SniiOssAdapter::boolean_or(const std::vector<std::string>& terms,
                                std::vector<uint32_t>* docids,
                                snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  if (snii::Status s = snii::query::boolean_or(*index_, terms, docids); !s.ok()) {
    fail("boolean_or", s);
  }
  *metrics = metered_->metrics();
}

void SniiOssAdapter::match_all(std::vector<uint32_t>* docids,
                               snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  const uint32_t n = require_uint32_doc_count(index_->stats().doc_count,
                                              "SNII OSS adapter");
  docids->resize(n);
  for (uint32_t i = 0; i < n; ++i) (*docids)[i] = i;
  *metrics = metered_->metrics();
}

void SniiOssAdapter::prefix_query(const std::string& prefix,
                                  std::vector<uint32_t>* docids,
                                  snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  if (snii::Status s = snii::query::prefix_query(*index_, prefix, docids); !s.ok()) {
    fail("prefix_query", s);
  }
  *metrics = metered_->metrics();
}

std::vector<std::string> SniiOssAdapter::enumerate_prefix(const std::string& prefix) {
  std::vector<snii::reader::LogicalIndexReader::PrefixHit> hits;
  if (snii::Status s = index_->prefix_terms(prefix, &hits); !s.ok()) {
    fail("prefix_terms", s);
  }
  std::vector<std::string> out;
  out.reserve(hits.size());
  for (auto& h : hits) out.push_back(std::move(h.term));
  return out;
}

void SniiOssAdapter::phrase_prefix_query(const std::vector<std::string>& fixed,
                                         const std::vector<std::string>& expansions,
                                         std::vector<uint32_t>* docids,
                                         snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  std::vector<uint32_t> acc;
  std::vector<std::string> phrase = fixed;
  phrase.push_back(std::string());
  for (const std::string& exp : expansions) {
    phrase.back() = exp;
    std::vector<uint32_t> d;
    if (snii::Status s = snii::query::phrase_query(*index_, phrase, &d); !s.ok()) {
      fail("phrase_prefix phrase_query", s);
    }
    std::vector<uint32_t> merged;
    merged.reserve(acc.size() + d.size());
    std::set_union(acc.begin(), acc.end(), d.begin(), d.end(),
                   std::back_inserter(merged));
    acc = std::move(merged);
  }
  *docids = std::move(acc);
  *metrics = metered_->metrics();
}

void SniiOssAdapter::phrase_prefix_query_prefix(
    const std::vector<std::string>& fixed, const std::string& prefix,
    std::vector<uint32_t>* docids, snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  std::vector<std::string> terms = fixed;
  terms.push_back(prefix);
  if (snii::Status s = snii::query::phrase_prefix_query(*index_, terms, docids);
      !s.ok()) {
    fail("phrase_prefix_query", s);
  }
  *metrics = metered_->metrics();
}

}  // namespace bench

#endif  // SNII_WITH_S3
