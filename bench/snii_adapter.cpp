#include "snii_adapter.h"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "snii/format/format_constants.h"
#include "snii/query/phrase_query.h"
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
  if (!path_.empty()) std::remove(path_.c_str());
}

void SniiAdapter::build_and_open(const Corpus& c) {
  using namespace snii;
  using snii::format::IndexConfig;
  using snii::writer::SniiCompoundWriter;
  using snii::writer::SniiIndexInput;
  using snii::writer::SpimiTermBuffer;
  using snii::writer::TermPostings;

  // 1. Accumulate every (term, doc, position) into the SPIMI buffer.
  SpimiTermBuffer buf(/*has_positions=*/true);
  for (uint32_t d = 0; d < c.doc_count; ++d) {
    const auto& toks = c.docs[d];
    for (uint32_t pos = 0; pos < toks.size(); ++pos) {
      buf.add_token(c.vocab[toks[pos]], d, pos);
    }
  }
  std::vector<TermPostings> terms = buf.finalize_sorted();

  // 2. Describe the single logical index (docid + freq + positions).
  SniiIndexInput in;
  in.index_id = 1;
  in.index_suffix = "body";
  in.config = IndexConfig::kDocsPositions;
  in.doc_count = c.doc_count;
  in.terms = std::move(terms);
  // A small DICT block target yields many blocks and a finer-grained sampled
  // term index, which keeps the term-dictionary lookup path accurate across the
  // whole (large) vocabulary.
  in.target_dict_block_bytes = 512;

  // 3. Write the compound container to a temp file.
  path_ = make_temp_path();
  io::LocalFileWriter writer;
  if (Status s = writer.open(path_); !s.ok()) fail("open writer", s);
  SniiCompoundWriter compound(&writer);
  if (Status s = compound.add_logical_index(in); !s.ok()) fail("add index", s);
  if (Status s = compound.finish(); !s.ok()) fail("finish writer", s);

  // 4. Open it through a metered local reader.
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

uint64_t SniiAdapter::index_bytes() const {
  if (path_.empty()) return 0;
  std::error_code ec;
  const auto sz = std::filesystem::file_size(path_, ec);
  return ec ? 0 : static_cast<uint64_t>(sz);
}

}  // namespace bench
