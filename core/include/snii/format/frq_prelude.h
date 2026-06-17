#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"

// FrqPrelude: columnar window-metadata directory that precedes windowed .frq
// postings. These bytes belong to the .frq payload domain (see design spec
// "window meta prelude" section): the windowed .frq payload is
// [prelude][frq window 0][frq window 1]... and DictEntry records prelude_len so
// a reader can range-fetch the prelude first.
//
// On-disk layout (strict):
//   header:
//     u8   ver               # format version (kFrqPreludeVersion)
//     u8   flags             # bit0 has_freq, bit1 has_prx, bit2 reserved (super-block, unused in v1)
//     VInt N                 # number of .frq windows
//     VInt M                 # number of .prx windows (present ONLY when has_prx)
//     VInt col_len[]         # byte length of each column that follows, in order
//     u32  crc32c            # covers header + all columns (everything before this u32)
//   columns (in order; col_len[] gives the byte length of each):
//     B  max_freq[N]         (varint32 stream)
//     B2 max_norm[N]         (u8 each)
//     C  last_docid_delta[N] (varint32 stream) — per-window absolute-docid anchor info
//     D  frq_window_len[N]   (varint32 stream) — per-window doc count; THIS is the
//                                                doc-count source for frq_pod reads at L3
//     E  prx_cum_off[M]      (varint64 stream) — present ONLY when has_prx
//     H  win_crc32c[N]       (fixed32 each)
//
// Column count is fixed: 5 columns when has_prx=false (B,B2,C,D,H), 6 when
// has_prx=true (B,B2,C,D,E,H). The trailing crc32c makes any corruption
// detectable; col_len[] lets the reader split the column region and self-check
// against the recorded counts (mismatch => Corruption).
//
// All multi-byte fixed-width fields are little-endian; variable-length integers
// reuse snii/encoding (ByteSink/ByteSource). Super-block (SB) directory is out
// of scope for v1: flags bit2 is reserved and never set.
namespace snii::format {

inline constexpr uint8_t kFrqPreludeVersion = 1;

namespace frq_prelude_flags {
inline constexpr uint8_t kHasFreq = 1u << 0;
inline constexpr uint8_t kHasPrx = 1u << 1;
inline constexpr uint8_t kHasSb = 1u << 2;  // reserved (super-block), never set in v1
}  // namespace frq_prelude_flags

// Per-window columns supplied by the writer. All per-frq-window vectors
// (max_freq, max_norm, last_docid_delta, frq_window_len, win_crc32c) must have
// the same length N. prx_cum_off has length M and is written only when has_prx.
struct FrqPreludeColumns {
  bool has_freq = true;
  bool has_prx = false;
  std::vector<uint32_t> max_freq;          // B  : per-window max term frequency
  std::vector<uint8_t> max_norm;           // B2 : per-window encoded norm for BM25 upper bound
  std::vector<uint32_t> last_docid_delta;  // C  : per-window absolute-docid anchor delta
  std::vector<uint32_t> frq_window_len;    // D  : per-window doc count
  std::vector<uint32_t> win_crc32c;        // H  : per-window crc32c
  std::vector<uint64_t> prx_cum_off;       // E  : per-prx-window cumulative offset
};

// Builds the prelude bytes and appends them to sink.
// Returns InvalidArgument when sink is null or the per-frq-window vectors have
// inconsistent lengths (or prx_cum_off is non-empty while has_prx is false).
Status build_frq_prelude(const FrqPreludeColumns& cols, ByteSink* sink);

// Reads and verifies a prelude buffer, then provides random access to every
// column. The reader copies all column values into owned vectors at open time
// (the prelude is small), so it does not retain the input Slice.
class FrqPreludeReader {
 public:
  // Parses the header, verifies the trailing crc32c, splits the column region by
  // col_len[], and decodes all columns.
  // crc mismatch / truncation / col_len inconsistency => kCorruption;
  // unsupported version => kUnsupported.
  static Status open(Slice prelude, FrqPreludeReader* out);

  uint32_t n_windows() const { return static_cast<uint32_t>(frq_window_len_.size()); }
  uint32_t m_prx_windows() const { return static_cast<uint32_t>(prx_cum_off_.size()); }
  bool has_freq() const { return has_freq_; }
  bool has_prx() const { return has_prx_; }

  // Per-frq-window accessors. An index >= n_windows() returns a defensive 0.
  uint32_t max_freq(uint32_t w) const { return at(max_freq_, w); }
  uint8_t max_norm(uint32_t w) const { return at(max_norm_, w); }
  uint32_t last_docid_delta(uint32_t w) const { return at(last_docid_delta_, w); }
  uint32_t frq_window_len(uint32_t w) const { return at(frq_window_len_, w); }
  uint32_t win_crc32c(uint32_t w) const { return at(win_crc32c_, w); }

  // Per-prx-window accessor. An index >= m_prx_windows() returns a defensive 0.
  uint64_t prx_cum_off(uint32_t m) const { return at(prx_cum_off_, m); }

 private:
  template <typename T>
  static T at(const std::vector<T>& v, uint32_t i) {
    return i < v.size() ? v[i] : T{};
  }

  bool has_freq_ = false;
  bool has_prx_ = false;
  std::vector<uint32_t> max_freq_;
  std::vector<uint8_t> max_norm_;
  std::vector<uint32_t> last_docid_delta_;
  std::vector<uint32_t> frq_window_len_;
  std::vector<uint32_t> win_crc32c_;
  std::vector<uint64_t> prx_cum_off_;
};

}  // namespace snii::format
