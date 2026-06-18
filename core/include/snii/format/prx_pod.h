#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"

// .prx position window (PrxPod): stores term position information for several docs within one window.
//
// Single-window on-disk byte layout (see docs/design SNII "prx design"):
//   u8   codec        # PrxCodec: 0=raw / 1=zstd / 2=pfor (bit7 cont-reserved)
//   VInt uncomp_len   # payload length (raw/pfor: on-disk payload bytes; zstd: plaintext)
//   VInt comp_len     # present only when codec==zstd
//   u32  crc32c       # covers header (codec..comp_len) + payload
//   bytes payload     # raw: varint plaintext; zstd: compressed; pfor: bit-packed
//
// raw/zstd plaintext payload (self-describing per-doc boundaries):
//   VInt doc_count
//   per doc: VInt pos_count, followed by pos_count position deltas (VInt)
//   positions within a doc are ascending, stored as deltas (first absolute).
//
// pfor payload (default build codec; no entropy coding):
//   VInt doc_count
//   VInt total_pos                   # sum of all pos_counts
//   per doc: VInt pos_count
//   PFOR_runs(position_deltas)       # total_pos deltas, kFrqBaseUnit per run,
//                                    #   flat doc order (first per doc absolute)
//
// Multi-byte fixed-length fields are little-endian; variable-length integers reuse snii/encoding/varint. crc32c checksum at window tail detects corruption.
namespace snii::format {

// Build a .prx window and append it to sink.
// per_doc_positions[d] is the position list for the d-th doc within this window; must be ascending (duplicates allowed).
// zstd_level_or_negative_for_auto:
//   <0  → auto: use ZSTD (default level) when payload is large enough, otherwise raw.
//   0   → force raw (no compression).
//   >0  → force ZSTD with the given level.
// Non-ascending positions within a doc return InvalidArgument.
Status build_prx_window(std::span<const std::vector<uint32_t>> per_doc_positions,
                        int zstd_level_or_negative_for_auto, ByteSink* sink);

// Vector convenience overload (forwards a span view over the window's per-doc
// lists; the writer can pass a slice of its flat positions WITHOUT deep-copying
// the inner vectors into a fresh std::vector<std::vector<uint32_t>> per window).
inline Status build_prx_window(
    const std::vector<std::vector<uint32_t>>& per_doc_positions,
    int zstd_level_or_negative_for_auto, ByteSink* sink) {
  return build_prx_window(std::span<const std::vector<uint32_t>>(per_doc_positions),
                          zstd_level_or_negative_for_auto, sink);
}

// FLAT-positions builder: byte-identical output to build_prx_window above, but
// reads the window's positions from a single flat span partitioned per-doc by
// `freqs` (doc d owns the next freqs[d] entries; freqs.size() == doc count and
// sum(freqs) == positions_flat.size()). Lets the writer pass a subspan of the
// term's flat positions/freqs with NO vector-of-vectors materialization.
Status build_prx_window_flat(std::span<const uint32_t> positions_flat,
                             std::span<const uint32_t> freqs,
                             int zstd_level_or_negative_for_auto, ByteSink* sink);

// Read and verify a .prx window from source, reconstructing the per-doc position list.
// CRC mismatch / invalid codec / truncation / decompression failure all return a non-OK Status.
Status read_prx_window(ByteSource* source,
                       std::vector<std::vector<uint32_t>>* per_doc_positions);

}  // namespace snii::format
