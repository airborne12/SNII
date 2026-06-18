#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/format/dict_entry.h"
#include "snii/format/frq_prelude.h"
#include "snii/reader/logical_index_reader.h"

// WindowedPostingReader -- shared read-side decode of a windowed term's full
// posting from its two-level frq_prelude + concatenated .frq (and .prx) windows.
//
// A windowed pod_ref entry's .frq payload is [prelude][win0 frq][win1 frq]...
// and its .prx payload is [win0 prx][win1 prx].... This helper:
//   1. range-fetches the prelude (prelude_len bytes) and parses the two-level
//      directory,
//   2. range-fetches every window's .frq (+.prx) sub-range in one batch,
//   3. decodes each window with its prelude-derived win_base and concatenates
//      the per-window docids / freqs / positions into the full posting.
//
// The slim/inline single-window path is handled by the term/phrase/scoring
// callers directly; this helper is for enc=windowed entries only.
namespace snii::reader {

// Coalesce gap (bytes) used when batch-fetching MULTIPLE windows of the SAME
// term: windows of one term are near-contiguous in the .frq POD, so merging
// reads separated by <= this gap into one physical Range GET trades a little
// over-read for fewer remote GETs (the design's higher-priority metric). Only
// applied to same-term multi-window batches, never to cross-term batches.
inline constexpr uint64_t kSameTermCoalesceGap = 16u * 1024u;

// Full decoded posting for one windowed term (docids ascending across windows).
struct DecodedPosting {
  std::vector<uint32_t> docids;
  std::vector<uint32_t> freqs;                   // aligned with docids
  std::vector<std::vector<uint32_t>> positions;  // aligned; empty when no prx
};

// Decodes the entire windowed posting. want_positions requires the index to
// have positions (and the entry to carry prx). want_freq selects whether the
// freq region of each .frq window is fetched + decoded: when false ONLY the
// docs-only prefix ([frq_off, frq_off+frq_docs_len)) is fetched and DecodedPosting
// .freqs stays empty (docid-only / phrase callers); when true the full window is
// fetched and per-doc freqs are decoded (scoring). Returns Corruption on any
// prelude/window inconsistency (doc-count mismatch, out-of-range offsets).
Status read_windowed_posting(const LogicalIndexReader& idx,
                             const snii::format::DictEntry& entry,
                             uint64_t frq_base, uint64_t prx_base,
                             bool want_positions, bool want_freq,
                             DecodedPosting* out);

// --- Sub-block (window) skipping helpers (shared with phrase_query) ---------
//
// These expose the per-window addressing + decode used by read_windowed_posting
// so the phrase-skip path can fetch ONLY the windows covering candidate docids
// (instead of the whole posting) without duplicating the offset arithmetic.

// Absolute file byte ranges of one window of a windowed entry. The .prx range is
// valid only when positions are wanted (and the entry carries prx).
struct WindowAbsRange {
  uint64_t frq_off = 0;
  uint64_t frq_len = 0;
  uint64_t prx_off = 0;
  uint64_t prx_len = 0;
};

// Fetches + parses the two-level prelude of a windowed entry (one batched read).
Status fetch_windowed_prelude(const LogicalIndexReader& idx,
                              const snii::format::DictEntry& entry,
                              uint64_t frq_base,
                              snii::format::FrqPreludeReader* prelude);

// Computes the absolute .frq (+ optional .prx) byte ranges of window w, fully
// validated against the POD sections (anti-DoS: rejects out-of-range offsets and
// overflowing locators). want_positions requires the prelude to carry prx.
// want_freq selects the .frq range length: when true the full window (frq_len),
// when false ONLY the docs-only prefix (frq_docs_len, validated <= frq_len) so
// the freq region never crosses the wire. The .prx range is unaffected.
Status windowed_window_range(const LogicalIndexReader& idx,
                             const snii::format::DictEntry& entry,
                             uint64_t frq_base, uint64_t prx_base,
                             const snii::format::FrqPreludeReader& prelude,
                             uint32_t w, bool want_positions, bool want_freq,
                             WindowAbsRange* out);

// Decodes one window's docids (and per-doc positions when want_positions, and
// per-doc freqs when want_freq) from already-fetched byte slices, using the
// window's prelude metadata. When want_freq is false the frq_window slice is the
// docs-only prefix (freq region absent) and freqs stays empty; when true it is
// the full window. The decoded docids are absolute (win_base applied). Returns
// Corruption on any doc-count mismatch between the prelude, .frq and .prx windows.
Status decode_window_slices(const snii::format::WindowMeta& meta,
                            Slice frq_window, Slice prx_window, bool want_positions,
                            bool want_freq, std::vector<uint32_t>* docids,
                            std::vector<uint32_t>* freqs,
                            std::vector<std::vector<uint32_t>>* positions);

}  // namespace snii::reader
