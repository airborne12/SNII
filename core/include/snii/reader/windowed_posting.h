#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/status.h"
#include "snii/format/dict_entry.h"
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

// Full decoded posting for one windowed term (docids ascending across windows).
struct DecodedPosting {
  std::vector<uint32_t> docids;
  std::vector<uint32_t> freqs;                   // aligned with docids
  std::vector<std::vector<uint32_t>> positions;  // aligned; empty when no prx
};

// Decodes the entire windowed posting. want_positions requires the index to
// have positions (and the entry to carry prx). Returns Corruption on any
// prelude/window inconsistency (doc-count mismatch, out-of-range offsets).
Status read_windowed_posting(const LogicalIndexReader& idx,
                             const snii::format::DictEntry& entry,
                             uint64_t frq_base, uint64_t prx_base,
                             bool want_positions, DecodedPosting* out);

}  // namespace snii::reader
