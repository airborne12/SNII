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
// SEPARABLE on-disk byte layout (dd region and freq region are independently
// encoded so a docs-only reader can fetch + decode just the dd prefix):
//   u8   flags         # bit0 dd_zstd, bit1 has_freq, bit2 freq_zstd
//   VInt dd_uncomp_len # dd region uncompressed byte count (written for raw too)
//   VInt dd_disk_len   # dd region on-disk byte count (== dd_uncomp_len when raw)
//   [VInt freq_uncomp_len   # present only when has_freq]
//   [VInt freq_disk_len     # present only when has_freq]
//   u32  crc_dd        # covers [flags .. dd_region] (header bytes + dd region)
//   bytes dd_region    # raw or zstd of dd_part
//   [u32  crc_freq     # present only when has_freq; covers freq_region]
//   [bytes freq_region # present only when has_freq; raw or zstd of freq_part]
//
// dd_part   = VInt n ++ PFOR_runs(doc_delta)   # n is the doc count, making the window self-describing
// freq_part = PFOR_runs(freq)                  # present only when has_freq=true
// PFOR runs are segmented at 256 docs (kFrqBaseUnit); a partial segment writes the remainder.
//
// frq_docs_len = byte length of the docs-only prefix [flags .. crc_dd .. dd_region].
// This prefix is an independently fetchable + decodable slice (read_frq_window_docs
// works given only these bytes, with the freq region absent). The full window
// length is frq_docs_len + [crc_freq + freq_region] when has_freq.
//
// Multi-byte fixed-width fields are little-endian; variable-length integers reuse snii/encoding/varint; PFOR reuses snii/encoding/pfor.
// Each region carries its own crc32c; corruption in either region is detectable.
namespace snii::format {

// Result of build_frq_window: the docs-only prefix length so the writer can
// record it (prelude row / slim DictEntry) for later wire-level freq-skipping.
struct FrqWindowLayout {
  uint64_t frq_docs_len = 0;  // [flags .. crc_dd .. dd_region] byte length
  uint64_t frq_len = 0;       // full window byte length (prefix + freq region)
};

// Build a .frq window and append it to sink.
// docids_ascending: ascending docids within this window (single doc or empty window allowed).
// freqs: must have the same length as docids when has_freq=true; ignored when has_freq=false (pass empty).
// win_base: base docid (first window=0, non-first window=last docid of the previous window); requires docids[0] >= win_base.
// zstd_level_or_neg_for_auto:
//   <0  → auto: use ZSTD (default level) when a region is large enough, otherwise raw (decided per region).
//   0   → force raw (no compression).
//   >0  → force ZSTD at the given level.
// out_layout (optional): receives the docs-only prefix length and full length.
// Non-ascending docids / freq length mismatch / first_docid < win_base / null sink returns InvalidArgument.
Status build_frq_window(const std::vector<uint32_t>& docids_ascending,
                        const std::vector<uint32_t>& freqs, uint64_t win_base, bool has_freq,
                        int zstd_level_or_neg_for_auto, ByteSink* sink,
                        FrqWindowLayout* out_layout = nullptr);

// docs-only path: decode ONLY the dd region to reconstruct docids, verifying
// crc_dd. Works even when the freq region bytes are absent from the slice (i.e.
// given only the docs-only prefix [flags .. crc_dd .. dd_region]).
// crc mismatch / invalid flags / truncation / decompression failure all return a non-OK Status.
Status read_frq_window_docs(ByteSource* source, uint64_t win_base,
                            std::vector<uint32_t>* docids);

// scoring path: decode both docids and freqs, verifying both crc_dd and crc_freq.
// freqs decoded from a has_freq=false window will be empty.
// crc mismatch / invalid flags / truncation / decompression failure all return a non-OK Status.
Status read_frq_window(ByteSource* source, uint64_t win_base, std::vector<uint32_t>* docids,
                       std::vector<uint32_t>* freqs);

}  // namespace snii::format
