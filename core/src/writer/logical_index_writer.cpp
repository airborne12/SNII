#include "snii/writer/logical_index_writer.h"

#include <algorithm>
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
using snii::format::WindowMeta;

namespace {

// Zstd "auto" sentinel for window builders (raw for tiny payloads).
constexpr int kAutoZstd = -1;
// Windows per super-block in the two-level prelude directory (design section 5).
constexpr uint32_t kPreludeGroupSize = 64;

// Builds a single .frq window (docs/freqs slice) with the given win_base.
Status MakeFrqWindow(const std::vector<uint32_t>& docids,
                     const std::vector<uint32_t>& freqs, uint64_t win_base,
                     std::vector<uint8_t>* out) {
  ByteSink sink;
  SNII_RETURN_IF_ERROR(snii::format::build_frq_window(
      docids, freqs, win_base, /*has_freq=*/true, kAutoZstd, &sink));
  *out = sink.buffer();
  return Status::OK();
}

// Builds a single .prx window from a slice of per-doc position lists.
Status MakePrxWindow(const std::vector<std::vector<uint32_t>>& positions,
                     std::vector<uint8_t>* out) {
  ByteSink sink;
  SNII_RETURN_IF_ERROR(snii::format::build_prx_window(positions, kAutoZstd, &sink));
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

// Builds the two-level .frq prelude for a windowed term and returns its bytes.
Status BuildPrelude(const std::vector<WindowMeta>& windows, bool has_prx,
                    std::vector<uint8_t>* out) {
  FrqPreludeColumns cols;
  cols.has_freq = true;
  cols.has_prx = has_prx;
  cols.group_size = kPreludeGroupSize;
  cols.windows = windows;
  ByteSink sink;
  SNII_RETURN_IF_ERROR(snii::format::build_frq_prelude(cols, &sink));
  *out = sink.buffer();
  return Status::OK();
}

void AppendBytes(std::vector<uint8_t>* dst, const std::vector<uint8_t>& src) {
  dst->insert(dst->end(), src.begin(), src.end());
}

// One windowed term's serialized windows: the concatenated frq/prx payloads and
// the per-window metadata (offsets relative to the start of each region).
struct WindowedPosting {
  std::vector<uint8_t> frq_bytes;          // [win0 frq][win1 frq]...
  std::vector<uint8_t> prx_bytes;          // [win0 prx][win1 prx]... (empty if !has_prx)
  std::vector<WindowMeta> windows;
};

// Splits a windowed term's postings into kFrqBaseUnit-sized windows, building
// each window's .frq (and .prx) payload and accumulating per-window metadata
// with offsets relative to the start of the frq/prx regions (window_start).
Status BuildWindowedPosting(const TermPostings& tp, bool has_prx,
                            WindowedPosting* out) {
  const uint32_t unit = snii::format::kFrqBaseUnit;
  const size_t n = tp.docids.size();
  uint64_t win_base = 0;  // absolute last docid of the previous window
  for (size_t start = 0; start < n; start += unit) {
    const size_t end = std::min(n, start + unit);
    const std::vector<uint32_t> docs(tp.docids.begin() + start, tp.docids.begin() + end);
    const std::vector<uint32_t> freqs(tp.freqs.begin() + start, tp.freqs.begin() + end);

    std::vector<uint8_t> frq_win;
    SNII_RETURN_IF_ERROR(MakeFrqWindow(docs, freqs, win_base, &frq_win));

    WindowMeta m;
    m.last_docid = docs.back();
    m.win_base = win_base;
    m.doc_count = static_cast<uint32_t>(docs.size());
    m.frq_off = static_cast<uint64_t>(out->frq_bytes.size());
    m.frq_len = static_cast<uint64_t>(frq_win.size());
    m.max_freq = MaxOf(freqs);
    m.max_norm = 0;
    m.win_crc = snii::crc32c(Slice(frq_win));
    AppendBytes(&out->frq_bytes, frq_win);

    if (has_prx) {
      const std::vector<std::vector<uint32_t>> pos(tp.positions.begin() + start,
                                                   tp.positions.begin() + end);
      std::vector<uint8_t> prx_win;
      SNII_RETURN_IF_ERROR(MakePrxWindow(pos, &prx_win));
      m.prx_off = static_cast<uint64_t>(out->prx_bytes.size());
      m.prx_len = static_cast<uint64_t>(prx_win.size());
      AppendBytes(&out->prx_bytes, prx_win);
    }
    out->windows.push_back(m);
    win_base = m.last_docid;
  }
  return Status::OK();
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

// Emits a windowed term: splits into kFrqBaseUnit windows, builds a two-level
// prelude, and lays out [prelude][win0 frq][win1 frq]... in the .frq POD and
// [win0 prx][win1 prx]... in the .prx POD. Sets enc=windowed + has_sb.
Status LogicalIndexWriter::build_windowed_entry(const TermPostings& tp, uint64_t frq_base,
                                                uint64_t prx_base, DictEntry* e) {
  WindowedPosting wp;
  SNII_RETURN_IF_ERROR(BuildWindowedPosting(tp, has_prx_, &wp));
  std::vector<uint8_t> prelude;
  SNII_RETURN_IF_ERROR(BuildPrelude(wp.windows, has_prx_, &prelude));

  e->kind = DictEntryKind::kPodRef;
  e->enc = DictEntryEnc::kWindowed;
  e->has_sb = true;  // prelude is always a two-level skip directory.
  e->prelude_len = static_cast<uint64_t>(prelude.size());

  const uint64_t frq_off = static_cast<uint64_t>(frq_pod_.size());
  AppendBytes(&frq_pod_, prelude);
  AppendBytes(&frq_pod_, wp.frq_bytes);
  e->frq_off_delta = frq_off - frq_base;
  e->frq_len = static_cast<uint64_t>(frq_pod_.size()) - frq_off;
  if (has_prx_) {
    const uint64_t prx_off = static_cast<uint64_t>(prx_pod_.size());
    AppendBytes(&prx_pod_, wp.prx_bytes);
    e->prx_off_delta = prx_off - prx_base;
    e->prx_len = static_cast<uint64_t>(prx_pod_.size()) - prx_off;
  }
  return Status::OK();
}

// Emits a slim term as a single .frq window (win_base=0): inline when the
// encoded bytes are tiny, otherwise a slim pod_ref (no prelude).
Status LogicalIndexWriter::build_slim_entry(const TermPostings& tp, uint64_t frq_base,
                                            uint64_t prx_base, DictEntry* e) {
  std::vector<uint8_t> frq_win;
  SNII_RETURN_IF_ERROR(MakeFrqWindow(tp.docids, tp.freqs, /*win_base=*/0, &frq_win));
  std::vector<uint8_t> prx_win;
  if (has_prx_) SNII_RETURN_IF_ERROR(MakePrxWindow(tp.positions, &prx_win));

  if (frq_win.size() <= snii::format::kDefaultInlineThreshold) {
    e->kind = DictEntryKind::kInline;
    e->enc = DictEntryEnc::kSlim;
    e->frq_bytes = std::move(frq_win);
    if (has_prx_) e->prx_bytes = std::move(prx_win);
    return Status::OK();
  }

  e->kind = DictEntryKind::kPodRef;
  e->enc = DictEntryEnc::kSlim;
  const uint64_t frq_off = static_cast<uint64_t>(frq_pod_.size());
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

// Builds the DictEntry for one term. Inline entries embed their .frq/.prx bytes;
// pod_ref entries append posting bytes to the PODs and record off_delta relative
// to frq_base/prx_base (the POD sizes captured when the block opened).
Status LogicalIndexWriter::build_entry(const TermPostings& tp, uint64_t frq_base,
                                       uint64_t prx_base, DictEntry* e) {
  e->term = tp.term;
  e->df = static_cast<uint32_t>(tp.docids.size());
  e->ttf_delta = SumOf(tp.freqs);  // simple: ttf stored directly as ttf_delta
  e->max_freq = MaxOf(tp.freqs);

  if (e->df >= snii::format::kSlimDfThreshold) {
    return build_windowed_entry(tp, frq_base, prx_base, e);
  }
  return build_slim_entry(tp, frq_base, prx_base, e);
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
