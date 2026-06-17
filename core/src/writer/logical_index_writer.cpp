#include "snii/writer/logical_index_writer.h"

#include <memory>
#include <utility>

#include "snii/common/slice.h"
#include "snii/encoding/crc32c.h"
#include "snii/format/dict_block.h"
#include "snii/format/frq_prelude.h"
#include "snii/format/frq_pod.h"
#include "snii/format/norms_pod.h"
#include "snii/format/prx_pod.h"
#include "snii/format/xfilter.h"

namespace snii::writer {

using snii::format::BlockRef;
using snii::format::DictBlockBuilder;
using snii::format::DictBlockDirectoryBuilder;
using snii::format::DictEntry;
using snii::format::DictEntryEnc;
using snii::format::DictEntryKind;
using snii::format::FrqPreludeColumns;
using snii::format::PerIndexMetaBuilder;
using snii::format::SampledTermIndexBuilder;
using snii::format::SectionRefs;

namespace {

// Zstd "auto" sentinel for window builders (raw for tiny payloads).
constexpr int kAutoZstd = -1;

Status MakeFrqWindow(const TermPostings& tp, std::vector<uint8_t>* out) {
  ByteSink sink;
  SNII_RETURN_IF_ERROR(snii::format::build_frq_window(
      tp.docids, tp.freqs, /*win_base=*/0, /*has_freq=*/true, kAutoZstd, &sink));
  *out = sink.buffer();
  return Status::OK();
}

Status MakePrxWindow(const TermPostings& tp, std::vector<uint8_t>* out) {
  ByteSink sink;
  SNII_RETURN_IF_ERROR(snii::format::build_prx_window(tp.positions, kAutoZstd, &sink));
  *out = sink.buffer();
  return Status::OK();
}

uint32_t MaxOf(const std::vector<uint32_t>& v) {
  uint32_t m = 0;
  for (uint32_t x : v) {
    if (x > m) m = x;
  }
  return m;
}

uint64_t SumOf(const std::vector<uint32_t>& v) {
  uint64_t s = 0;
  for (uint32_t x : v) s += x;
  return s;
}

// Builds a single-window .frq prelude for a windowed term and returns its bytes.
Status BuildPrelude(const TermPostings& tp, const std::vector<uint8_t>& frq_win,
                    bool has_prx, std::vector<uint8_t>* out) {
  FrqPreludeColumns cols;
  cols.has_freq = true;
  cols.has_prx = has_prx;
  cols.max_freq = {MaxOf(tp.freqs)};
  cols.max_norm = {0};
  cols.last_docid_delta = {tp.docids.empty() ? 0u : tp.docids.back()};
  cols.frq_window_len = {static_cast<uint32_t>(tp.docids.size())};
  cols.win_crc32c = {snii::crc32c(Slice(frq_win))};
  if (has_prx) cols.prx_cum_off = {0};
  ByteSink sink;
  SNII_RETURN_IF_ERROR(snii::format::build_frq_prelude(cols, &sink));
  *out = sink.buffer();
  return Status::OK();
}

void AppendBytes(std::vector<uint8_t>* dst, const std::vector<uint8_t>& src) {
  dst->insert(dst->end(), src.begin(), src.end());
}

}  // namespace

LogicalIndexWriter::LogicalIndexWriter(const SniiIndexInput& in)
    : index_id_(in.index_id),
      index_suffix_(in.index_suffix),
      tier_(snii::format::tier_of(in.config)),
      has_prx_(snii::format::has_positions(in.config)),
      has_norms_(snii::format::has_scoring(in.config)),
      doc_count_(in.doc_count),
      terms_(in.terms),
      encoded_norms_(in.encoded_norms),
      target_dict_block_bytes_(in.target_dict_block_bytes != 0
                                   ? in.target_dict_block_bytes
                                   : snii::format::kDefaultTargetDictBlockBytes) {}

Status LogicalIndexWriter::validate() const {
  if (has_norms_ && encoded_norms_.size() != doc_count_) {
    return Status::InvalidArgument("logical_index: norms length must equal doc_count");
  }
  for (const auto& tp : terms_) {
    if (tp.freqs.size() != tp.docids.size()) {
      return Status::InvalidArgument("logical_index: freqs length must equal docids");
    }
    if (has_prx_ && tp.positions.size() != tp.docids.size()) {
      return Status::InvalidArgument("logical_index: positions length must equal docids");
    }
    for (size_t i = 1; i < tp.docids.size(); ++i) {
      if (tp.docids[i] <= tp.docids[i - 1]) {
        return Status::InvalidArgument("logical_index: docids must be strictly ascending");
      }
    }
  }
  return Status::OK();
}

// Builds the DictEntry for one term. Inline entries embed their .frq/.prx bytes;
// pod_ref entries append posting bytes to the PODs and record off_delta relative
// to frq_base/prx_base (the POD sizes captured when the block opened).
Status LogicalIndexWriter::build_entry(const TermPostings& tp, uint64_t frq_base,
                                       uint64_t prx_base, DictEntry* e) {
  e->term = tp.term;
  e->df = static_cast<uint32_t>(tp.docids.size());
  e->ttf_delta = SumOf(tp.freqs);  // simple: ttf stored directly as ttf_delta
  e->max_freq = MaxOf(tp.freqs);

  std::vector<uint8_t> frq_win;
  SNII_RETURN_IF_ERROR(MakeFrqWindow(tp, &frq_win));
  std::vector<uint8_t> prx_win;
  if (has_prx_) SNII_RETURN_IF_ERROR(MakePrxWindow(tp, &prx_win));

  const bool windowed = e->df >= snii::format::kSlimDfThreshold;
  if (!windowed && frq_win.size() <= snii::format::kDefaultInlineThreshold) {
    e->kind = DictEntryKind::kInline;
    e->enc = DictEntryEnc::kSlim;
    e->frq_bytes = std::move(frq_win);
    if (has_prx_) e->prx_bytes = std::move(prx_win);
    return Status::OK();
  }

  e->kind = DictEntryKind::kPodRef;
  e->enc = windowed ? DictEntryEnc::kWindowed : DictEntryEnc::kSlim;
  const uint64_t frq_off = static_cast<uint64_t>(frq_pod_.size());
  if (windowed) {
    std::vector<uint8_t> prelude;
    SNII_RETURN_IF_ERROR(BuildPrelude(tp, frq_win, has_prx_, &prelude));
    e->prelude_len = static_cast<uint64_t>(prelude.size());
    AppendBytes(&frq_pod_, prelude);
  }
  AppendBytes(&frq_pod_, frq_win);
  e->frq_off_delta = frq_off - frq_base;
  e->frq_len = static_cast<uint64_t>(frq_pod_.size()) - frq_off;
  if (has_prx_) {
    const uint64_t prx_off = static_cast<uint64_t>(prx_pod_.size());
    AppendBytes(&prx_pod_, prx_win);
    e->prx_off_delta = prx_off - prx_base;
    e->prx_len = static_cast<uint64_t>(prx_pod_.size()) - prx_off;
  }
  return Status::OK();
}

void LogicalIndexWriter::flush_block(DictBlockBuilder* block, std::string first_term) {
  ByteSink bsink;
  block->finish(&bsink);
  BlockOut out;
  out.bytes = bsink.buffer();
  out.n_entries = block->n_entries();
  out.checksum = snii::crc32c(Slice(out.bytes));
  out.first_term = first_term;
  sample_first_terms_.push_back(std::move(first_term));
  blocks_.push_back(std::move(out));
}

Status LogicalIndexWriter::build_blocks() {
  std::unique_ptr<DictBlockBuilder> block;
  std::string block_first_term;
  uint64_t frq_base = 0;
  uint64_t prx_base = 0;

  for (const auto& tp : terms_) {
    all_terms_.push_back(tp.term);
    stats_.sum_total_term_freq += SumOf(tp.freqs);

    if (!block) {
      frq_base = static_cast<uint64_t>(frq_pod_.size());
      prx_base = static_cast<uint64_t>(prx_pod_.size());
      block = std::make_unique<DictBlockBuilder>(tier_, has_prx_, frq_base, prx_base);
      block_first_term = tp.term;
    }

    DictEntry e;
    SNII_RETURN_IF_ERROR(build_entry(tp, frq_base, prx_base, &e));
    block->add_entry(e);

    if (block->estimated_bytes() >= target_dict_block_bytes_) {
      flush_block(block.get(), block_first_term);
      block.reset();
    }
  }
  if (block) flush_block(block.get(), block_first_term);
  return Status::OK();
}

Status LogicalIndexWriter::build() {
  SNII_RETURN_IF_ERROR(validate());
  SNII_RETURN_IF_ERROR(build_blocks());

  for (const auto& b : blocks_) AppendBytes(&dict_region_, b.bytes);

  stats_.doc_count = doc_count_;
  stats_.indexed_doc_count = doc_count_;
  stats_.term_count = static_cast<uint64_t>(terms_.size());
  stats_.null_count = 0;

  if (has_norms_) {
    snii::format::NormsPodWriter nw;
    for (uint8_t n : encoded_norms_) nw.add(n);
    ByteSink nsink;
    nw.finish(&nsink);
    norms_section_ = nsink.buffer();
  }

  ByteSink xsink;
  SNII_RETURN_IF_ERROR(snii::format::build_xfilter(all_terms_, &xsink));
  xfilter_bytes_ = xsink.buffer();
  return Status::OK();
}

Status LogicalIndexWriter::finish_meta(const SectionRefs& abs_refs,
                                       uint64_t dict_region_offset,
                                       ByteSink* out) const {
  if (out == nullptr) return Status::InvalidArgument("logical_index: null meta sink");

  SampledTermIndexBuilder sti;
  for (const auto& t : sample_first_terms_) sti.add_block_first_term(t);
  ByteSink sti_sink;
  sti.finish(&sti_sink);

  DictBlockDirectoryBuilder dir;
  uint64_t running = dict_region_offset;
  for (const auto& b : blocks_) {
    BlockRef ref;
    ref.offset = running;
    ref.length = static_cast<uint64_t>(b.bytes.size());
    ref.n_entries = b.n_entries;
    ref.flags = 0;
    ref.checksum = b.checksum;
    dir.add(ref);
    running += b.bytes.size();
  }
  ByteSink dir_sink;
  dir.finish(&dir_sink);

  PerIndexMetaBuilder builder(index_id_, index_suffix_,
                              PerIndexMetaBuilder::kHasXFilter);
  builder.set_stats(stats_);
  builder.set_sampled_term_index(sti_sink.view());
  builder.set_dict_block_directory(dir_sink.view());
  builder.set_xfilter(Slice(xfilter_bytes_));
  builder.set_section_refs(abs_refs);
  return builder.finish(out);
}

}  // namespace snii::writer
