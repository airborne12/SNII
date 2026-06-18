#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"

// XFilter -- binary-fuse-8 filter for fast absent-term rejection (EXACT term only).
//
// Located in the per-index meta block. The filter is immutable and built once from the full term set at
// finish time, which fits an immutable segment well. It only answers exact-term membership: a query term
// either is "definitely absent" or "maybe present"; there are no false negatives, and the false-positive
// rate is ~0.4%. Range / regexp / prefix / phrase_prefix queries must be expanded via the sorted term
// enumeration and cannot be pruned with this filter. See design spec "absent-term fast filter".
//
// Construction is the standard binary fuse 8 algorithm built FROM SCRATCH: a 3-wise XOR "fuse" filter with
// 8-bit fingerprints, constructed by peeling. Each term is hashed to a 64-bit key via xxHash (XXH3_64bits).
//
// On-disk layout (framed by SectionFramer, uniform type+len+crc32c):
//   [u8 type=kXFilter][varint64 payload_len][payload][fixed32 crc32c]
//   payload =
//     seed                  u64 (fixed64, little-endian)
//     segment_length        u32 (fixed32)
//     segment_count_length  u32 (fixed32)
//     array_length          u32 (fixed32)
//     fingerprints          u8[array_length]
namespace snii::format {

// One-shot build: hashes each term, dedups internally, constructs the binary-fuse-8 filter (retrying with a
// new seed if peeling fails), and appends a single kXFilter framed section to sink. Input terms may be
// unsorted and may contain duplicates. An empty term set produces a valid, always-absent filter.
// Returns kInvalidArgument if sink is null; kInternal only if peeling fails after the retry budget.
Status build_xfilter(const std::vector<std::string>& terms, ByteSink* sink);

// XXH3_64bits hash of a term -- the SAME key the filter is built/queried with. Exposed so a writer can
// collect one 8-byte key per term DURING the build (folding the per-term string copy away) instead of
// retaining the whole vocabulary just to feed build_xfilter at finish.
uint64_t hash_term(std::string_view term);

// Pre-hashed build: identical to build_xfilter but takes term hashes (from hash_term) directly. The keys
// are sorted+deduped internally, so the caller may pass them unsorted and with duplicates. Producing the
// exact same on-disk bytes as build_xfilter over the same term set (same dedup, same params, same seed
// search). `keys` is consumed (moved-from) to avoid a second whole-vocabulary copy.
Status build_xfilter_hashed(std::vector<uint64_t> keys, ByteSink* sink);

// Reader: verifies the framing checksum and materializes the fingerprint table on open; subsequent queries
// are pure in-memory lookups.
class XFilterReader {
 public:
  XFilterReader() = default;

  // Parses a kXFilter framed section.
  // CRC mismatch / truncation / field overrun → kCorruption; type != kXFilter → kInvalidArgument;
  // out == nullptr → kInvalidArgument.
  static Status open(Slice section, XFilterReader* out);

  // Membership query. Returns false → term is definitely absent; true → term may be present.
  // Guarantees no false negatives for any term used at build time.
  bool maybe_contains(std::string_view term) const;

  // Size of the fingerprint table (array_length). Zero for an empty (no-term) filter.
  uint32_t fingerprint_count() const { return static_cast<uint32_t>(fingerprints_.size()); }

 private:
  uint64_t seed_ = 0;
  uint32_t segment_length_ = 0;
  uint32_t segment_count_length_ = 0;
  std::vector<uint8_t> fingerprints_;
};

}  // namespace snii::format
