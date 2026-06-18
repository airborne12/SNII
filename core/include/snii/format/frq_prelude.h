#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"

// FrqPrelude: a TWO-LEVEL (super-block -> window) skippable directory that
// precedes a windowed .frq posting. These bytes belong to the .frq payload
// domain (see design spec section 4): the windowed .frq payload is
//   [prelude][frq window 0][frq window 1]...
// and DictEntry records prelude_len so a reader can range-fetch the prelude
// first, then range-fetch only the windows that cover candidate docids.
//
// On-disk layout (strict; all multi-byte fixed fields little-endian, VInt =
// LEB128 via snii/encoding):
//   header:
//     u8   flags        # bit0 has_freq, bit1 has_prx
//     VInt N            # number of .frq windows
//     VInt G            # windows per super-block (group_size; >=1)
//     VInt n_super      # = ceil(N / G); 0 when N==0
//     VInt sbdir_len    # byte length of the super_block_dir region
//     u32  crc32c       # covers header + super_block_dir (NOT the window blocks)
//   super_block_dir[n_super]:  # small, resident: one row per super-block
//     VInt sb_last_docid_delta # cumulative across super-blocks => absolute last
//                              #   docid of the super-block's last window
//     VInt sb_block_off        # byte offset of this super-block's window block,
//                              #   measured from the start of the window_dir
//                              #   region (i.e. relative to the byte right after
//                              #   the trailing crc of the super_block_dir)
//     VInt sb_block_len        # byte length of this super-block's window block
//   window_dir: n_super self-contained blocks, each holding <=G window rows.
//     per window row:
//       VInt last_docid_delta  # cumulative WITHIN the block => absolute last
//                              #   docid; the previous window's absolute last
//                              #   docid is win_base (first window of first
//                              #   block: win_base = 0)
//       VInt doc_count         # number of docs in the window (frq_pod needs it)
//       VInt frq_off           # byte offset of the window .frq payload within
//                              #   the .frq region, relative to window_start
//                              #   (window_start = entry.frq_off + prelude_len)
//       VInt frq_len           # byte length of the window .frq payload
//       VInt prx_off           # .prx payload byte offset (present iff has_prx)
//       VInt prx_len           # .prx payload byte length (present iff has_prx)
//       VInt max_freq          # window max term frequency (WAND block-max)
//       u8   max_norm          # window score-max norm (WAND); 0 acceptable
//       u32  win_crc32c        # crc32c of the window .frq payload bytes
//
// Reconstructing win_base / absolute last_docid (READER CONTRACT):
//   The writer computes, for every window in term order, the running absolute
//   last docid. Within a super-block's window block, each row stores the delta
//   of its absolute last docid from the PREVIOUS window's absolute last docid
//   (the previous window may live in the previous block; the first window of the
//   whole term uses 0 as its previous). super_block_dir.sb_last_docid is that
//   running value at the super-block boundary, so the reader can seed each
//   block's cumulative sum from the previous super-block's sb_last_docid. Thus:
//     win_base(w)      = absolute last_docid(w-1)  (0 for w==0)
//     last_docid(w)    = win_base(w) + last_docid_delta(w)
//   This makes super-block binary search (over sb_last_docid) followed by
//   in-block window binary search (over last_docid) locate the window covering
//   any docid without decoding the .frq windows.
//
// The trailing crc32c only covers header + super_block_dir; every window row
// carries its own win_crc32c, and the .frq window payload carries its own crc.
namespace snii::format {

namespace frq_prelude_flags {
inline constexpr uint8_t kHasFreq = 1u << 0;
inline constexpr uint8_t kHasPrx = 1u << 1;
}  // namespace frq_prelude_flags

// Absolute, decoded metadata for one window (as the reader exposes it).
struct WindowMeta {
  uint32_t last_docid = 0;  // absolute last docid in the window
  uint64_t win_base = 0;    // absolute last docid of the previous window (0 for w==0)
  uint32_t doc_count = 0;
  uint64_t frq_off = 0;  // relative to window_start (= entry.frq_off + prelude_len)
  uint64_t frq_len = 0;
  uint64_t prx_off = 0;  // valid only when has_prx
  uint64_t prx_len = 0;  // valid only when has_prx
  uint32_t max_freq = 0;
  uint8_t max_norm = 0;
  uint32_t win_crc = 0;
};

// Builder input: one fully-computed WindowMeta per window, in term order, plus
// the super-block grouping factor. The writer fills last_docid (absolute),
// doc_count, the offsets/lens, max_freq, max_norm and win_crc; win_base is
// derived during build (so callers may leave it 0). group_size must be >= 1.
struct FrqPreludeColumns {
  bool has_freq = true;
  bool has_prx = false;
  uint32_t group_size = 64;  // windows per super-block (G)
  std::vector<WindowMeta> windows;
};

// Builds the prelude bytes and appends them to out.
// Returns InvalidArgument when out is null, group_size is 0, or the windows are
// not in non-decreasing last_docid order (a window's absolute last docid must be
// >= the previous window's).
Status build_frq_prelude(const FrqPreludeColumns& cols, ByteSink* out);

// Reads and verifies a prelude buffer, exposing two-level skip access. The
// reader parses the header + super_block_dir on open (verifying the trailing
// crc) and eagerly decodes every window block into owned WindowMeta rows (the
// prelude is small relative to the postings). It does not retain the input.
class FrqPreludeReader {
 public:
  // Parses + verifies the prelude. crc mismatch / truncation / inconsistent
  // offsets-or-lengths / oversized counts => kCorruption.
  static Status open(Slice prelude, FrqPreludeReader* out);

  uint32_t n_windows() const { return static_cast<uint32_t>(windows_.size()); }
  uint32_t n_super_blocks() const { return n_super_; }
  bool has_freq() const { return has_freq_; }
  bool has_prx() const { return has_prx_; }

  // Returns the absolute WindowMeta for window w. Out-of-range => InvalidArgument.
  Status window(uint32_t w, WindowMeta* out) const;

  // Locates the window covering docid via super-block binary search then window
  // binary search. *found=false (with OK) when docid is past the term's last
  // docid; otherwise *w is the index of the covering window (the first window
  // whose absolute last_docid >= docid).
  Status locate_window(uint32_t docid, bool* found, uint32_t* w) const;

 private:
  bool has_freq_ = false;
  bool has_prx_ = false;
  uint32_t group_size_ = 1;
  uint32_t n_super_ = 0;
  // Absolute last docid at each super-block boundary (size n_super_).
  std::vector<uint64_t> sb_last_docid_;
  // All windows decoded with absolute fields, in term order (size N).
  std::vector<WindowMeta> windows_;
};

}  // namespace snii::format
