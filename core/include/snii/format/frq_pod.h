#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"

// .frq window (FrqPod): doc/freq postings data, columnar + PFOR (see docs/design SNII "frq design").
//
// A window contains n ascending docs (base unit=256, combinable as 256/512/1024/2048). win_base is provided by the caller:
// first window=0, non-first window=last docid of the previous window. dd[0]=first_docid-win_base; dd[i]=docid[i]-docid[i-1].
//
// Single-window on-disk byte layout:
//   u8   win_mode      # FrqWinMode: 0=raw / 1=zstd
//   VInt uncomp_len    # uncompressed payload byte count (written for raw too)
//   VInt comp_len      # present only when win_mode==kZstd
//   VInt dd_part_len   # byte length of the dd section within payload; bitmap-only path uses this to skip freq
//   u32  crc32c        # covers header (win_mode..dd_part_len) + payload
//   bytes payload(decompressed) = dd_part ++ freq_part
//
// Decompressed payload has two sections (columnar: all doc_deltas first, then all freqs):
//   dd_part   = VInt n ++ PFOR_runs(doc_delta)   # n is the doc count, making the window self-describing
//   freq_part = PFOR_runs(freq)                  # empty when has_freq=false
// PFOR runs are segmented at 256 docs (kFrqBaseUnit); a partial segment writes the remainder.
// docs-only (has_freq=false): freq_part is empty, dd_part_len==uncomp_len.
//
// Multi-byte fixed-width fields are little-endian; variable-length integers reuse snii/encoding/varint; PFOR reuses snii/encoding/pfor.
// The trailing crc32c covers header+payload; corruption is detectable.
namespace snii::format {

// Build a .frq window and append it to sink.
// docids_ascending: ascending docids within this window (single doc or empty window allowed).
// freqs: must have the same length as docids when has_freq=true; ignored when has_freq=false (pass empty).
// win_base: base docid (first window=0, non-first window=last docid of the previous window); requires docids[0] >= win_base.
// zstd_level_or_neg_for_auto:
//   <0  → auto: use ZSTD (default level) when payload is large enough, otherwise raw.
//   0   → force raw (no compression).
//   >0  → force ZSTD at the given level.
// Non-ascending docids / freq length mismatch / first_docid < win_base / null sink returns InvalidArgument.
Status build_frq_window(const std::vector<uint32_t>& docids_ascending,
                        const std::vector<uint32_t>& freqs, uint64_t win_base, bool has_freq,
                        int zstd_level_or_neg_for_auto, ByteSink* sink);

// bitmap-only path: decode only the dd section to reconstruct docids, skip freq via dd_part_len (raw/zstd both supported).
// crc mismatch / invalid win_mode / truncation / decompression failure all return a non-OK Status.
Status read_frq_window_docs(ByteSource* source, uint64_t win_base,
                            std::vector<uint32_t>* docids);

// scoring path: decode both docids and freqs simultaneously.
// freqs decoded from a has_freq=false window will be empty.
// crc mismatch / invalid win_mode / truncation / decompression failure all return a non-OK Status.
Status read_frq_window(ByteSource* source, uint64_t win_base, std::vector<uint32_t>* docids,
                       std::vector<uint32_t>* freqs);

}  // namespace snii::format
