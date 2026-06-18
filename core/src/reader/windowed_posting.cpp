#include "snii/reader/windowed_posting.h"

#include <cstddef>
#include <vector>

#include "snii/common/slice.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/frq_pod.h"
#include "snii/format/frq_prelude.h"
#include "snii/format/prx_pod.h"
#include "snii/io/batch_range_fetcher.h"

namespace snii::reader {

using snii::format::DictEntry;
using snii::format::FrqPreludeReader;
using snii::format::WindowMeta;

namespace {

// Resolves the absolute file offset of the prelude bytes for a windowed entry.
uint64_t PreludeAbs(const LogicalIndexReader& idx, const DictEntry& entry,
                    uint64_t frq_base) {
  const auto& frq = idx.section_refs().frq_pod;
  return frq.offset + frq_base + entry.frq_off_delta;
}

// Validates that [off, off+len) fits within [0, total).
Status InBounds(uint64_t off, uint64_t len, uint64_t total) {
  if (off > total || len > total - off) {
    return Status::Corruption("windowed_posting: window range out of section");
  }
  return Status::OK();
}

// Per-window batch handles for the planned .frq (+.prx) sub-range reads.
struct WindowPlan {
  WindowMeta meta;
  size_t frq_handle = 0;
  size_t prx_handle = 0;
};

// Plans (registers) every window's .frq and optional .prx sub-range against the
// fetcher, validating each range against its section length.
Status PlanWindows(const LogicalIndexReader& idx, const DictEntry& entry,
                   uint64_t frq_base, uint64_t prx_base, bool want_positions,
                   const FrqPreludeReader& prelude,
                   snii::io::BatchRangeFetcher* fetcher,
                   std::vector<WindowPlan>* plans) {
  const uint32_t n = prelude.n_windows();
  plans->reserve(n);
  for (uint32_t w = 0; w < n; ++w) {
    WindowPlan p;
    SNII_RETURN_IF_ERROR(prelude.window(w, &p.meta));
    WindowAbsRange r;
    SNII_RETURN_IF_ERROR(windowed_window_range(idx, entry, frq_base, prx_base,
                                               prelude, w, want_positions, &r));
    p.frq_handle = fetcher->add(r.frq_off, static_cast<size_t>(r.frq_len));
    if (want_positions) {
      p.prx_handle = fetcher->add(r.prx_off, static_cast<size_t>(r.prx_len));
    }
    plans->push_back(p);
  }
  return Status::OK();
}

// Decodes one window's docids/freqs (and positions) and appends to the output.
Status AppendWindow(const snii::io::BatchRangeFetcher& fetcher,
                    const WindowPlan& p, bool want_positions, DecodedPosting* out) {
  std::vector<uint32_t> docids;
  std::vector<uint32_t> freqs;
  std::vector<std::vector<uint32_t>> pos;
  const Slice prx = want_positions ? fetcher.get(p.prx_handle) : Slice();
  SNII_RETURN_IF_ERROR(decode_window_slices(p.meta, fetcher.get(p.frq_handle), prx,
                                            want_positions, &docids, &freqs, &pos));
  out->docids.insert(out->docids.end(), docids.begin(), docids.end());
  out->freqs.insert(out->freqs.end(), freqs.begin(), freqs.end());
  if (want_positions) {
    for (auto& v : pos) out->positions.push_back(std::move(v));
  }
  return Status::OK();
}

}  // namespace

Status fetch_windowed_prelude(const LogicalIndexReader& idx, const DictEntry& entry,
                              uint64_t frq_base, FrqPreludeReader* prelude) {
  if (entry.prelude_len == 0) {
    return Status::Corruption("windowed_posting: windowed entry has no prelude");
  }
  if (entry.prelude_len > entry.frq_len) {
    return Status::Corruption("windowed_posting: prelude_len exceeds frq_len");
  }
  const uint64_t prelude_abs = PreludeAbs(idx, entry, frq_base);
  snii::io::BatchRangeFetcher fetcher(idx.reader());
  const size_t h = fetcher.add(prelude_abs, static_cast<size_t>(entry.prelude_len));
  SNII_RETURN_IF_ERROR(fetcher.fetch());
  return FrqPreludeReader::open(fetcher.get(h), prelude);
}

Status windowed_window_range(const LogicalIndexReader& idx, const DictEntry& entry,
                             uint64_t frq_base, uint64_t prx_base,
                             const FrqPreludeReader& prelude, uint32_t w,
                             bool want_positions, WindowAbsRange* out) {
  if (out == nullptr) return Status::InvalidArgument("windowed_posting: null range");
  // Self-defend (the skip path reaches here without the prelude-fetch validation):
  // prelude_len > frq_len would underflow frq_region_len below.
  if (entry.prelude_len > entry.frq_len) {
    return Status::Corruption("windowed_posting: prelude_len exceeds frq_len");
  }
  WindowMeta meta;
  SNII_RETURN_IF_ERROR(prelude.window(w, &meta));

  // .frq windows live after the prelude; validate against the post-prelude span.
  const uint64_t frq_window_start = PreludeAbs(idx, entry, frq_base) + entry.prelude_len;
  const uint64_t frq_region_len = entry.frq_len - entry.prelude_len;
  SNII_RETURN_IF_ERROR(InBounds(meta.frq_off, meta.frq_len, frq_region_len));
  out->frq_off = frq_window_start + meta.frq_off;
  out->frq_len = meta.frq_len;

  if (!want_positions) {
    out->prx_off = 0;
    out->prx_len = 0;
    return Status::OK();
  }
  if (!prelude.has_prx()) {
    return Status::Corruption("windowed_posting: positions requested but prelude has none");
  }
  const uint64_t prx_region_start =
      idx.section_refs().prx_pod.offset + prx_base + entry.prx_off_delta;
  SNII_RETURN_IF_ERROR(InBounds(meta.prx_off, meta.prx_len, entry.prx_len));
  out->prx_off = prx_region_start + meta.prx_off;
  out->prx_len = meta.prx_len;
  return Status::OK();
}

Status decode_window_slices(const WindowMeta& meta, Slice frq_window, Slice prx_window,
                            bool want_positions, std::vector<uint32_t>* docids,
                            std::vector<uint32_t>* freqs,
                            std::vector<std::vector<uint32_t>>* positions) {
  ByteSource fsrc(frq_window);
  SNII_RETURN_IF_ERROR(
      snii::format::read_frq_window(&fsrc, meta.win_base, docids, freqs));
  if (docids->size() != meta.doc_count) {
    return Status::Corruption("windowed_posting: frq doc_count mismatch");
  }
  if (!want_positions) return Status::OK();

  ByteSource psrc(prx_window);
  SNII_RETURN_IF_ERROR(snii::format::read_prx_window(&psrc, positions));
  if (positions->size() != docids->size()) {
    return Status::Corruption("windowed_posting: prx/frq doc-count mismatch");
  }
  return Status::OK();
}

Status read_windowed_posting(const LogicalIndexReader& idx, const DictEntry& entry,
                             uint64_t frq_base, uint64_t prx_base,
                             bool want_positions, DecodedPosting* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("windowed_posting: null out");
  }
  *out = DecodedPosting{};

  FrqPreludeReader prelude;
  SNII_RETURN_IF_ERROR(fetch_windowed_prelude(idx, entry, frq_base, &prelude));
  if (want_positions && !prelude.has_prx()) {
    return Status::Corruption("windowed_posting: positions requested but prelude has none");
  }

  snii::io::BatchRangeFetcher fetcher(idx.reader());
  std::vector<WindowPlan> plans;
  SNII_RETURN_IF_ERROR(PlanWindows(idx, entry, frq_base, prx_base, want_positions,
                                   prelude, &fetcher, &plans));
  if (fetcher.pending() > 0) SNII_RETURN_IF_ERROR(fetcher.fetch());

  for (const WindowPlan& p : plans) {
    SNII_RETURN_IF_ERROR(AppendWindow(fetcher, p, want_positions, out));
  }
  return Status::OK();
}

}  // namespace snii::reader
