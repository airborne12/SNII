#include "snii/format/frq_pod.h"

#include <cstddef>

#include "snii/common/slice.h"
#include "snii/encoding/crc32c.h"
#include "snii/encoding/pfor.h"
#include "snii/encoding/zstd_codec.h"
#include "snii/format/format_constants.h"

namespace snii::format {
namespace {

// Auto-compression threshold: use raw when a region is smaller than this byte count (zstd gain is negligible and metadata overhead is relatively large).
inline constexpr size_t kAutoZstdMinBytes = 512;
// Default zstd level for auto mode.
inline constexpr int kDefaultZstdLevel = 3;
// Maximum decompressed byte size for a single region. Guards against a corrupted uncomp_len read from S3 that inflated to a huge value:
// sanity-check before allocating/decompressing to avoid GB-scale allocations. Windows are 256-doc aligned and normally far smaller than this.
inline constexpr uint32_t kMaxRegionUncompBytes = 256u * 1024 * 1024;
// Maximum doc count per .frq window (guards against a corrupted n). Window baseline is 256, practical combined cap is 2048,
// so this is a loose but astronomically-large-number-blocking upper bound.
inline constexpr uint32_t kMaxWindowDocs = 1u << 24;

// flags byte bit layout (self-describing window header).
inline constexpr uint8_t kFlagDdZstd = 1u << 0;
inline constexpr uint8_t kFlagHasFreq = 1u << 1;
inline constexpr uint8_t kFlagFreqZstd = 1u << 2;
inline constexpr uint8_t kFlagKnownBits = kFlagDdZstd | kFlagHasFreq | kFlagFreqZstd;

// Encode a uint32 array into multiple PFOR runs, each of 256 (kFrqBaseUnit) elements.
// n / run count is not written: the number of runs is derived from total length n and kFrqBaseUnit, and the decoder computes it the same way.
void encode_pfor_runs(const std::vector<uint32_t>& values, ByteSink* out) {
  size_t n = values.size();
  for (size_t off = 0; off < n; off += kFrqBaseUnit) {
    size_t run = (n - off < kFrqBaseUnit) ? (n - off) : kFrqBaseUnit;
    pfor_encode(values.data() + off, run, out);
  }
}

// Decode n uint32 values from source (multiple PFOR runs of 256 each).
Status decode_pfor_runs(ByteSource* src, size_t n, std::vector<uint32_t>* out) {
  out->assign(n, 0);
  for (size_t off = 0; off < n; off += kFrqBaseUnit) {
    size_t run = (n - off < kFrqBaseUnit) ? (n - off) : kFrqBaseUnit;
    SNII_RETURN_IF_ERROR(pfor_decode(src, run, out->data() + off));
  }
  return Status::OK();
}

// Verify that docids are ascending, the first entry is not below win_base, and freq length matches.
Status validate_inputs(const std::vector<uint32_t>& docs, const std::vector<uint32_t>& freqs,
                       uint64_t win_base, bool has_freq) {
  if (has_freq && freqs.size() != docs.size()) {
    return Status::InvalidArgument("frq: freqs length must equal docids length");
  }
  if (docs.empty()) return Status::OK();
  if (static_cast<uint64_t>(docs.front()) < win_base) {
    return Status::InvalidArgument("frq: first docid below win_base");
  }
  for (size_t i = 1; i < docs.size(); ++i) {
    if (docs[i] < docs[i - 1]) {
      return Status::InvalidArgument("frq: docids must be ascending");
    }
  }
  return Status::OK();
}

// Encode the dd_part plaintext: VInt n ++ PFOR_runs(doc_delta).
void encode_dd_part(const std::vector<uint32_t>& docs, uint64_t win_base, ByteSink* out) {
  std::vector<uint32_t> dd(docs.size());
  uint64_t prev = win_base;
  for (size_t i = 0; i < docs.size(); ++i) {
    dd[i] = static_cast<uint32_t>(static_cast<uint64_t>(docs[i]) - prev);
    prev = docs[i];
  }
  out->put_varint32(static_cast<uint32_t>(docs.size()));
  encode_pfor_runs(dd, out);
}

// Decision: given level and plaintext length, determine whether to compress.
bool should_compress(int level, size_t plain_len) {
  if (level == 0) return false;           // force raw
  if (level > 0) return true;             // force zstd
  return plain_len >= kAutoZstdMinBytes;  // auto
}

// A single encoded region: the on-disk bytes plus the encode metadata.
struct EncodedRegion {
  bool zstd = false;
  uint64_t uncomp_len = 0;
  std::vector<uint8_t> disk;  // raw plaintext or compressed bytes
};

// Encodes one region's plaintext into raw or zstd according to level/size.
Status encode_region(Slice plain, int level, EncodedRegion* out) {
  out->uncomp_len = plain.size();
  if (!should_compress(level, plain.size())) {
    out->zstd = false;
    out->disk.assign(plain.data(), plain.data() + plain.size());
    return Status::OK();
  }
  out->zstd = true;
  return zstd_compress(plain, level > 0 ? level : kDefaultZstdLevel, &out->disk);
}

// Assembles the docs-only prefix [flags .. crc_dd .. dd_region] into sink and
// returns its byte length. The flags byte and both region header columns are
// covered by crc_dd, so the prefix verifies independently of the freq region.
uint64_t write_docs_prefix(uint8_t flags, const EncodedRegion& dd,
                           const EncodedRegion* freq, ByteSink* sink) {
  ByteSink header;  // covered by crc_dd alongside the dd region bytes
  header.put_u8(flags);
  header.put_varint64(dd.uncomp_len);
  header.put_varint64(static_cast<uint64_t>(dd.disk.size()));
  if (freq != nullptr) {
    header.put_varint64(freq->uncomp_len);
    header.put_varint64(static_cast<uint64_t>(freq->disk.size()));
  }
  ByteSink covered;
  covered.put_bytes(header.view());
  covered.put_bytes(Slice(dd.disk));
  const size_t before = sink->size();
  sink->put_bytes(covered.view());
  sink->put_fixed32(crc32c(covered.view()));
  return static_cast<uint64_t>(sink->size() - before);
}

// Appends the freq region (crc_freq + freq bytes) after the docs prefix.
void write_freq_region(const EncodedRegion& freq, ByteSink* sink) {
  sink->put_bytes(Slice(freq.disk));
  sink->put_fixed32(crc32c(Slice(freq.disk)));
}

// Region header columns parsed from the flags byte.
struct RegionHeader {
  bool zstd = false;
  uint64_t uncomp_len = 0;
  uint64_t disk_len = 0;
};

bool flags_valid(uint8_t flags) { return (flags & ~kFlagKnownBits) == 0; }

// Materializes a region's plaintext (raw borrows the view; zstd decompresses).
Status materialize_region(bool zstd, uint64_t uncomp_len, Slice disk,
                          std::vector<uint8_t>* holder, Slice* plain) {
  if (!zstd) {
    *plain = disk;
    return Status::OK();
  }
  SNII_RETURN_IF_ERROR(zstd_decompress(disk, static_cast<size_t>(uncomp_len), holder));
  *plain = Slice(*holder);
  return Status::OK();
}

// Reads the flags byte + dd/freq header columns + dd region, verifies crc_dd,
// and returns the dd region header and its raw on-disk slice. The freq header
// columns are also captured so read_frq_window can locate the freq region.
Status read_docs_prefix(ByteSource* src, uint8_t* flags, RegionHeader* dd,
                        RegionHeader* freq, Slice* dd_disk) {
  const size_t start = src->position();
  SNII_RETURN_IF_ERROR(src->get_u8(flags));
  if (!flags_valid(*flags)) return Status::Corruption("frq: unknown flags");
  dd->zstd = (*flags & kFlagDdZstd) != 0;
  SNII_RETURN_IF_ERROR(src->get_varint64(&dd->uncomp_len));
  SNII_RETURN_IF_ERROR(src->get_varint64(&dd->disk_len));
  const bool has_freq = (*flags & kFlagHasFreq) != 0;
  if (has_freq) {
    freq->zstd = (*flags & kFlagFreqZstd) != 0;
    SNII_RETURN_IF_ERROR(src->get_varint64(&freq->uncomp_len));
    SNII_RETURN_IF_ERROR(src->get_varint64(&freq->disk_len));
  }
  if (dd->uncomp_len > kMaxRegionUncompBytes || freq->uncomp_len > kMaxRegionUncompBytes) {
    return Status::Corruption("frq: region uncomp_len exceeds sane cap");
  }
  SNII_RETURN_IF_ERROR(src->get_bytes(static_cast<size_t>(dd->disk_len), dd_disk));
  const size_t covered_len = src->position() - start;
  uint32_t stored = 0;
  SNII_RETURN_IF_ERROR(src->get_fixed32(&stored));
  if (crc32c(src->slice_from(start, covered_len)) != stored) {
    return Status::Corruption("frq: dd region crc mismatch");
  }
  return Status::OK();
}

// Decode the dd region: read n and doc_delta from the plaintext and reconstruct ascending docids.
Status decode_docs(Slice dd_plain, uint64_t win_base, std::vector<uint32_t>* docids) {
  ByteSource src(dd_plain);
  uint32_t n = 0;
  SNII_RETURN_IF_ERROR(src.get_varint32(&n));
  if (n > kMaxWindowDocs) return Status::Corruption("frq: doc count exceeds sane cap");
  std::vector<uint32_t> dd;
  SNII_RETURN_IF_ERROR(decode_pfor_runs(&src, n, &dd));
  docids->assign(n, 0);
  uint64_t cur = win_base;
  for (uint32_t i = 0; i < n; ++i) {
    cur += dd[i];
    (*docids)[i] = static_cast<uint32_t>(cur);
  }
  return Status::OK();
}

// Reads + verifies the freq region (crc_freq) and decodes doc_count freqs.
Status read_and_decode_freqs(ByteSource* src, const RegionHeader& freq, size_t doc_count,
                             std::vector<uint32_t>* freqs) {
  Slice disk;
  SNII_RETURN_IF_ERROR(src->get_bytes(static_cast<size_t>(freq.disk_len), &disk));
  uint32_t stored = 0;
  SNII_RETURN_IF_ERROR(src->get_fixed32(&stored));
  if (crc32c(disk) != stored) return Status::Corruption("frq: freq region crc mismatch");
  if (doc_count == 0) {
    freqs->clear();
    return Status::OK();
  }
  std::vector<uint8_t> holder;
  Slice plain;
  SNII_RETURN_IF_ERROR(materialize_region(freq.zstd, freq.uncomp_len, disk, &holder, &plain));
  ByteSource fsrc(plain);
  return decode_pfor_runs(&fsrc, doc_count, freqs);
}

}  // namespace

Status build_frq_window(const std::vector<uint32_t>& docids_ascending,
                        const std::vector<uint32_t>& freqs, uint64_t win_base, bool has_freq,
                        int zstd_level_or_neg_for_auto, ByteSink* sink,
                        FrqWindowLayout* out_layout) {
  if (sink == nullptr) return Status::InvalidArgument("frq: null sink");
  SNII_RETURN_IF_ERROR(validate_inputs(docids_ascending, freqs, win_base, has_freq));

  ByteSink dd_plain;
  encode_dd_part(docids_ascending, win_base, &dd_plain);
  EncodedRegion dd;
  SNII_RETURN_IF_ERROR(encode_region(dd_plain.view(), zstd_level_or_neg_for_auto, &dd));

  EncodedRegion freq;
  if (has_freq) {
    ByteSink freq_plain;
    encode_pfor_runs(freqs, &freq_plain);
    SNII_RETURN_IF_ERROR(encode_region(freq_plain.view(), zstd_level_or_neg_for_auto, &freq));
  }

  uint8_t flags = 0;
  if (dd.zstd) flags |= kFlagDdZstd;
  if (has_freq) {
    flags |= kFlagHasFreq;
    if (freq.zstd) flags |= kFlagFreqZstd;
  }

  const size_t window_start = sink->size();
  const uint64_t docs_len =
      write_docs_prefix(flags, dd, has_freq ? &freq : nullptr, sink);
  if (has_freq) write_freq_region(freq, sink);
  if (out_layout != nullptr) {
    out_layout->frq_docs_len = docs_len;
    out_layout->frq_len = static_cast<uint64_t>(sink->size() - window_start);
  }
  return Status::OK();
}

Status read_frq_window_docs(ByteSource* source, uint64_t win_base,
                            std::vector<uint32_t>* docids) {
  if (source == nullptr || docids == nullptr) {
    return Status::InvalidArgument("frq: null arg");
  }
  uint8_t flags = 0;
  RegionHeader dd, freq;
  Slice dd_disk;
  SNII_RETURN_IF_ERROR(read_docs_prefix(source, &flags, &dd, &freq, &dd_disk));
  std::vector<uint8_t> holder;
  Slice dd_plain;
  SNII_RETURN_IF_ERROR(materialize_region(dd.zstd, dd.uncomp_len, dd_disk, &holder, &dd_plain));
  return decode_docs(dd_plain, win_base, docids);
}

Status read_frq_window(ByteSource* source, uint64_t win_base, std::vector<uint32_t>* docids,
                       std::vector<uint32_t>* freqs) {
  if (source == nullptr || docids == nullptr || freqs == nullptr) {
    return Status::InvalidArgument("frq: null arg");
  }
  uint8_t flags = 0;
  RegionHeader dd, freq;
  Slice dd_disk;
  SNII_RETURN_IF_ERROR(read_docs_prefix(source, &flags, &dd, &freq, &dd_disk));
  std::vector<uint8_t> holder;
  Slice dd_plain;
  SNII_RETURN_IF_ERROR(materialize_region(dd.zstd, dd.uncomp_len, dd_disk, &holder, &dd_plain));
  SNII_RETURN_IF_ERROR(decode_docs(dd_plain, win_base, docids));
  if ((flags & kFlagHasFreq) == 0) {
    freqs->clear();
    return Status::OK();
  }
  return read_and_decode_freqs(source, freq, docids->size(), freqs);
}

}  // namespace snii::format
