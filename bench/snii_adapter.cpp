#include "snii_adapter.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "snii/format/format_constants.h"
#include "snii/query/bm25_scorer.h"
#include "snii/query/phrase_query.h"
#include "snii/query/scoring_query.h"
#include "snii/query/term_query.h"
#include "snii/writer/snii_compound_writer.h"
#include "snii/writer/spimi_term_buffer.h"

namespace bench {
namespace {

std::string make_temp_path() {
  static int counter = 0;
  return "/tmp/snii_bench_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter++) + ".idx";
}

[[noreturn]] void fail(const std::string& what, const snii::Status& s) {
  throw std::runtime_error("SNII adapter: " + what + ": " + s.to_string());
}

}  // namespace

SniiAdapter::~SniiAdapter() {
  if (!path_.empty() && !keep_path_) std::remove(path_.c_str());
}

void SniiAdapter::build_and_open(const Corpus& c) {
  build_at(make_temp_path(), c, /*keep_on_disk=*/false);
}

std::vector<std::string> SniiAdapter::index_files() const {
  if (path_.empty()) return {};
  return {path_};
}

void SniiAdapter::build_at(const std::string& path, const Corpus& c,
                           bool keep_on_disk) {
  build_range(path, c, 0, c.doc_count, keep_on_disk);
}

void SniiAdapter::build_range(const std::string& path, const Corpus& c,
                              uint32_t doc_lo, uint32_t doc_hi,
                              bool keep_on_disk) {
  using namespace snii;
  using snii::format::IndexConfig;
  using snii::writer::SniiCompoundWriter;
  using snii::writer::SniiIndexInput;
  using snii::writer::SpimiTermBuffer;
  using snii::writer::TermPostings;

  if (doc_hi > c.doc_count || doc_lo > doc_hi) {
    throw std::runtime_error("SNII adapter: build_range bad [lo,hi)");
  }
  const uint32_t seg_docs = doc_hi - doc_lo;

  // 1. Accumulate every (term-id, LOCAL doc, position) for docs in [doc_lo,doc_hi)
  // into the SPIMI buffer. Local docids are d - doc_lo so this segment's docid
  // space is 0..seg_docs-1; the caller maps back to the global docid by adding
  // doc_lo. The corpus tokens are ALREADY dense ids into c.vocab (shared across
  // segments, no copy). A non-zero spill threshold bounds input RAM.
  // Keyword (docs-only) build stores no positions; tokenized stores positions.
  SpimiTermBuffer buf(&c.vocab, /*has_positions=*/!docs_only_, spill_threshold_bytes_);
  for (uint32_t d = doc_lo; d < doc_hi; ++d) {
    const auto& toks = c.docs[d];
    for (uint32_t pos = 0; pos < toks.size(); ++pos) {
      buf.add_token(toks[pos], d - doc_lo, pos);
    }
  }

  // 2. Describe the single logical index (docid + freq + positions). Feed terms
  // by STREAMING the SPIMI buffer so the writer materializes only one term's
  // postings at a time (low peak memory) instead of a full TermPostings vector.
  SniiIndexInput in;
  in.index_id = 1;
  in.index_suffix = "body";
  in.config = docs_only_  ? IndexConfig::kDocsOnly
              : scoring_   ? IndexConfig::kDocsPositionsScoring
                           : IndexConfig::kDocsPositions;
  in.doc_count = seg_docs;
  in.term_source = &buf;
  // A scoring index needs per-doc length norms (BM25 length normalization); the
  // doc length is its token count in [doc_lo, doc_hi).
  if (scoring_) {
    in.encoded_norms.resize(seg_docs);
    for (uint32_t d = doc_lo; d < doc_hi; ++d)
      in.encoded_norms[d - doc_lo] = snii::query::encode_norm(
          static_cast<uint32_t>(c.docs[d].size()));
  }
  // A larger DICT block target (64 KiB) yields far fewer blocks: it shrinks the
  // dict_block_directory (one BlockRef/block) and the sampled_term_index (one
  // sampled term/block) on disk, AND cuts the writer's per-block accumulation
  // (blocks_ + sample_first_terms_) -- a meaningful merge-phase RSS line at 5M
  // (953K terms => ~191K blocks at 512 B vs ~1.6K blocks at 64 KiB). Query lookup
  // reads a larger dict block per term, but the I/O-metric thesis is unaffected.
  in.target_dict_block_bytes = 64 * 1024;

  // 3. Write the compound container to `path` (creating parent dirs first).
  path_ = path;
  keep_path_ = keep_on_disk;
  if (const auto parent = std::filesystem::path(path_).parent_path();
      !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) throw std::runtime_error("SNII adapter: mkdir " + parent.string() +
                                     ": " + ec.message());
  }
  io::LocalFileWriter writer;
  if (Status s = writer.open(path_); !s.ok()) fail("open writer", s);
  SniiCompoundWriter compound(&writer);
  if (Status s = compound.add_logical_index(in); !s.ok()) fail("add index", s);
  if (Status s = compound.finish(); !s.ok()) fail("finish writer", s);

  // 4. Open it through a metered local reader.
  open_reader();
}

void SniiAdapter::build_multi(const std::string& path,
                             const std::vector<LogicalSpec>& specs,
                             bool keep_on_disk) {
  using namespace snii;
  using snii::format::IndexConfig;
  using snii::writer::SniiCompoundWriter;
  using snii::writer::SniiIndexInput;
  using snii::writer::SpimiTermBuffer;

  if (specs.empty()) throw std::runtime_error("SNII adapter: build_multi no specs");
  path_ = path;
  keep_path_ = keep_on_disk;
  if (const auto parent = std::filesystem::path(path_).parent_path();
      !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) throw std::runtime_error("SNII adapter: mkdir " + parent.string() +
                                     ": " + ec.message());
  }
  io::LocalFileWriter writer;
  if (Status s = writer.open(path_); !s.ok()) fail("open writer", s);
  SniiCompoundWriter compound(&writer);
  // Each logical index gets its own SPIMI buffer; add_logical_index builds it
  // immediately (consumes term_source), so the buffer is freed before the next.
  uint32_t index_id = 1;
  for (const LogicalSpec& spec : specs) {
    const Corpus& c = *spec.corpus;
    SpimiTermBuffer buf(&c.vocab, /*has_positions=*/!spec.docs_only,
                        spill_threshold_bytes_);
    for (uint32_t d = 0; d < c.doc_count; ++d) {
      const auto& toks = c.docs[d];
      for (uint32_t pos = 0; pos < toks.size(); ++pos) buf.add_token(toks[pos], d, pos);
    }
    SniiIndexInput in;
    in.index_id = index_id++;
    in.index_suffix = spec.suffix;
    in.config = spec.docs_only ? IndexConfig::kDocsOnly : IndexConfig::kDocsPositions;
    in.doc_count = c.doc_count;
    in.term_source = &buf;
    in.target_dict_block_bytes = 64 * 1024;
    if (Status s = compound.add_logical_index(in); !s.ok())
      fail("add index " + spec.suffix, s);
  }
  if (Status s = compound.finish(); !s.ok()) fail("finish writer", s);
  open_reader();  // opens index_id 1; use open_logical for others
}

void SniiAdapter::open_logical(uint32_t index_id, const std::string& suffix) {
  using namespace snii;
  if (segment_ == nullptr) throw std::runtime_error("SNII adapter: no open segment");
  index_ = std::make_unique<reader::LogicalIndexReader>();
  if (Status s = segment_->open_index(index_id, suffix, index_.get()); !s.ok()) {
    fail("open logical index " + suffix, s);
  }
}

void SniiAdapter::open_existing(const std::string& path) {
  path_ = path;
  keep_path_ = true;  // we did not create it; never delete it on destruction
  open_reader();
}

void SniiAdapter::open_reader() {
  using namespace snii;
  local_ = std::make_unique<io::LocalFileReader>();
  if (Status s = local_->open(path_); !s.ok()) fail("open reader", s);
  metered_ = std::make_unique<io::MeteredFileReader>(local_.get());

  segment_ = std::make_unique<reader::SniiSegmentReader>();
  if (Status s = reader::SniiSegmentReader::open(metered_.get(), segment_.get());
      !s.ok()) {
    fail("open segment", s);
  }
  index_ = std::make_unique<reader::LogicalIndexReader>();
  if (Status s = segment_->open_index(1, "body", index_.get()); !s.ok()) {
    fail("open logical index", s);
  }
  // A scoring index carries norms; materialize the resident stats provider once so
  // per-query scoring reads only postings (norms are resident metadata).
  stats_.reset();
  if (scoring_) {
    stats_ = std::make_unique<snii::stats::SniiStatsProvider>();
    if (Status s = snii::stats::SniiStatsProvider::open(index_.get(), stats_.get());
        !s.ok()) {
      fail("open stats provider", s);
    }
  }
}

void SniiAdapter::score_query(const std::vector<std::string>& terms, uint32_t k,
                              ScorePath path, std::vector<ScoredHit>* out,
                              snii::io::IoMetrics* metrics) {
  using namespace snii;
  if (stats_ == nullptr) {
    throw std::runtime_error("SNII adapter: score_query needs a scoring index");
  }
  metered_->reset_metrics();
  out->clear();
  const query::Bm25Params params;  // k1=1.2, b=0.75
  std::vector<query::ScoredDoc> hits;
  Status s;
  switch (path) {
    case ScorePath::kExhaustive:
      s = query::scoring_query_exhaustive(*index_, *stats_, terms, k, params, &hits);
      break;
    case ScorePath::kWand:
      s = query::scoring_query_wand(*index_, *stats_, terms, k, params, &hits);
      break;
    case ScorePath::kWandSelective:
      s = query::scoring_query_wand_selective(*index_, *stats_, terms, k, params, &hits);
      break;
  }
  if (!s.ok()) fail("score_query", s);
  out->reserve(hits.size());
  for (const auto& h : hits) out->push_back({h.docid, h.score});
  *metrics = metered_->metrics();
}

void SniiAdapter::term_query(const std::string& term,
                             std::vector<uint32_t>* docids,
                             snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  if (snii::Status s = snii::query::term_query(*index_, term, docids); !s.ok()) {
    fail("term_query(" + term + ")", s);
  }
  *metrics = metered_->metrics();
}

void SniiAdapter::phrase_query(const std::vector<std::string>& words,
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

void SniiAdapter::boolean_and(const std::vector<std::string>& terms,
                              std::vector<uint32_t>* docids,
                              snii::io::IoMetrics* metrics) {
  metered_->reset_metrics();
  docids->clear();
  if (snii::Status s = snii::query::boolean_and(*index_, terms, docids); !s.ok()) {
    fail("boolean_and", s);
  }
  *metrics = metered_->metrics();
}

void SniiAdapter::boolean_or(const std::vector<std::string>& terms,
                             std::vector<uint32_t>* docids,
                             snii::io::IoMetrics* metrics) {
  // One cold measurement covering the whole disjunction: read each term's posting
  // (no reset between terms) and union the docid sets.
  metered_->reset_metrics();
  docids->clear();
  std::vector<uint32_t> acc;
  for (const std::string& t : terms) {
    std::vector<uint32_t> d;
    if (snii::Status s = snii::query::term_query(*index_, t, &d); !s.ok()) {
      fail("boolean_or(" + t + ")", s);
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

void SniiAdapter::match_all(std::vector<uint32_t>* docids,
                            snii::io::IoMetrics* metrics) {
  // Every docid from the resident doc count -- no posting I/O.
  metered_->reset_metrics();
  const uint32_t n = index_->stats().doc_count;
  docids->resize(n);
  for (uint32_t i = 0; i < n; ++i) (*docids)[i] = i;
  *metrics = metered_->metrics();
}

uint64_t SniiAdapter::index_bytes() const {
  if (path_.empty()) return 0;
  std::error_code ec;
  const auto sz = std::filesystem::file_size(path_, ec);
  return ec ? 0 : static_cast<uint64_t>(sz);
}

}  // namespace bench
