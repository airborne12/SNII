#include "snii/format/frq_pod.h"

#include <cstddef>

#include "snii/common/slice.h"
#include "snii/encoding/crc32c.h"
#include "snii/encoding/pfor.h"
#include "snii/encoding/zstd_codec.h"
#include "snii/format/format_constants.h"

namespace snii::format {
namespace {

// Auto-compression threshold: use raw when payload is smaller than this byte count (zstd gain is negligible and metadata overhead is relatively large).
inline constexpr size_t kAutoZstdMinBytes = 512;
// Default zstd level for auto mode.
inline constexpr int kDefaultZstdLevel = 3;
// Maximum decompressed byte size for a single .frq window. Guards against a corrupted uncomp_len read from S3 that inflated to a huge value:
// sanity-check before allocating/decompressing to avoid GB-scale allocations. Windows are 256-doc aligned and normally far smaller than this.
inline constexpr uint32_t kMaxWindowUncompBytes = 256u * 1024 * 1024;
// Maximum doc count per .frq window (guards against a corrupted n). Window baseline is 256, practical combined cap is 2048,
// so this is a loose but astronomically-large-number-blocking upper bound.
inline constexpr uint32_t kMaxWindowDocs = 1u << 24;

bool is_valid_win_mode(uint8_t v) {
  return v == static_cast<uint8_t>(FrqWinMode::kRaw) ||
         v == static_cast<uint8_t>(FrqWinMode::kZstd);
}

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

// Encode the plaintext payload: dd_part(=VInt n ++ PFOR_runs(doc_delta)) ++ freq_part(=PFOR_runs(freq)).
// Returns the byte length of dd_part (used as dd_part_len in the header).
size_t encode_payload(const std::vector<uint32_t>& docs, const std::vector<uint32_t>& freqs,
                      uint64_t win_base, bool has_freq, ByteSink* out) {
  std::vector<uint32_t> dd(docs.size());
  uint64_t prev = win_base;
  for (size_t i = 0; i < docs.size(); ++i) {
    dd[i] = static_cast<uint32_t>(static_cast<uint64_t>(docs[i]) - prev);
    prev = docs[i];
  }
  out->put_varint32(static_cast<uint32_t>(docs.size()));
  encode_pfor_runs(dd, out);
  size_t dd_part_len = out->size();
  if (has_freq) encode_pfor_runs(freqs, out);
  return dd_part_len;
}

// Decision: given level and plaintext length, determine whether to compress.
bool should_compress(int level, size_t plain_len) {
  if (level == 0) return false;           // force raw
  if (level > 0) return true;             // force zstd
  return plain_len >= kAutoZstdMinBytes;  // auto
}

// Write a raw window: win_mode=raw, uncomp_len, dd_part_len, crc(header+payload), payload.
void write_raw(Slice plain, size_t dd_part_len, ByteSink* sink) {
  ByteSink framed;
  framed.put_u8(static_cast<uint8_t>(FrqWinMode::kRaw));
  framed.put_varint32(static_cast<uint32_t>(plain.size()));
  framed.put_varint32(static_cast<uint32_t>(dd_part_len));
  framed.put_bytes(plain);
  sink->put_bytes(framed.view());
  sink->put_fixed32(crc32c(framed.view()));
}

// Write a zstd window: win_mode=zstd, uncomp_len, comp_len, dd_part_len, crc(header+payload), payload.
Status write_zstd(Slice plain, size_t dd_part_len, int level, ByteSink* sink) {
  std::vector<uint8_t> comp;
  SNII_RETURN_IF_ERROR(zstd_compress(plain, level > 0 ? level : kDefaultZstdLevel, &comp));
  ByteSink framed;
  framed.put_u8(static_cast<uint8_t>(FrqWinMode::kZstd));
  framed.put_varint32(static_cast<uint32_t>(plain.size()));
  framed.put_varint32(static_cast<uint32_t>(comp.size()));
  framed.put_varint32(static_cast<uint32_t>(dd_part_len));
  framed.put_bytes(Slice(comp));
  sink->put_bytes(framed.view());
  sink->put_fixed32(crc32c(framed.view()));
  return Status::OK();
}

// Parsed window header and payload view.
struct WindowFrame {
  uint8_t win_mode = 0;
  uint32_t uncomp_len = 0;
  uint32_t dd_part_len = 0;
  Slice payload;  // plaintext for raw; compressed bytes for zstd
};

// Read header + payload, verify crc by rewinding, and return the header fields and payload view to the caller.
Status read_framed(ByteSource* src, WindowFrame* f) {
  size_t start = src->position();
  SNII_RETURN_IF_ERROR(src->get_u8(&f->win_mode));
  if (!is_valid_win_mode(f->win_mode)) return Status::Corruption("frq: unknown win_mode");
  SNII_RETURN_IF_ERROR(src->get_varint32(&f->uncomp_len));
  if (f->uncomp_len > kMaxWindowUncompBytes) {
    return Status::Corruption("frq: uncomp_len exceeds sane window cap");
  }
  size_t payload_len = f->uncomp_len;
  if (f->win_mode == static_cast<uint8_t>(FrqWinMode::kZstd)) {
    uint32_t comp_len = 0;
    SNII_RETURN_IF_ERROR(src->get_varint32(&comp_len));
    payload_len = comp_len;
  }
  SNII_RETURN_IF_ERROR(src->get_varint32(&f->dd_part_len));
  if (f->dd_part_len > f->uncomp_len) {
    return Status::Corruption("frq: dd_part_len exceeds uncomp_len");
  }
  SNII_RETURN_IF_ERROR(src->get_bytes(payload_len, &f->payload));
  size_t framed_len = src->position() - start;
  uint32_t stored = 0;
  SNII_RETURN_IF_ERROR(src->get_fixed32(&stored));
  if (crc32c(src->slice_from(start, framed_len)) != stored) {
    return Status::Corruption("frq: window crc mismatch");
  }
  return Status::OK();
}

// Decode the dd section: read n and doc_delta from the plaintext dd_part and reconstruct ascending docids.
Status decode_docs(Slice dd_part, uint64_t win_base, std::vector<uint32_t>* docids) {
  ByteSource src(dd_part);
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

// Decode the freq section: the plaintext after dd_part contains PFOR_runs(freq) for doc_count entries.
Status decode_freqs(Slice freq_part, size_t doc_count, std::vector<uint32_t>* freqs) {
  if (freq_part.empty()) {
    freqs->clear();
    return Status::OK();
  }
  ByteSource src(freq_part);
  return decode_pfor_runs(&src, doc_count, freqs);
}

// Obtain the plaintext payload (raw borrows the view directly; zstd decompresses into holder).
Status materialize_plain(const WindowFrame& f, std::vector<uint8_t>* holder, Slice* plain) {
  if (f.win_mode == static_cast<uint8_t>(FrqWinMode::kRaw)) {
    *plain = f.payload;
    return Status::OK();
  }
  SNII_RETURN_IF_ERROR(zstd_decompress(f.payload, f.uncomp_len, holder));
  *plain = Slice(*holder);
  return Status::OK();
}

}  // namespace

Status build_frq_window(const std::vector<uint32_t>& docids_ascending,
                        const std::vector<uint32_t>& freqs, uint64_t win_base, bool has_freq,
                        int zstd_level_or_neg_for_auto, ByteSink* sink) {
  if (sink == nullptr) return Status::InvalidArgument("frq: null sink");
  SNII_RETURN_IF_ERROR(validate_inputs(docids_ascending, freqs, win_base, has_freq));

  ByteSink plain;
  size_t dd_part_len =
      encode_payload(docids_ascending, freqs, win_base, has_freq, &plain);
  Slice plain_view = plain.view();
  if (!should_compress(zstd_level_or_neg_for_auto, plain_view.size())) {
    write_raw(plain_view, dd_part_len, sink);
    return Status::OK();
  }
  return write_zstd(plain_view, dd_part_len, zstd_level_or_neg_for_auto, sink);
}

Status read_frq_window_docs(ByteSource* source, uint64_t win_base,
                            std::vector<uint32_t>* docids) {
  if (source == nullptr || docids == nullptr) {
    return Status::InvalidArgument("frq: null arg");
  }
  WindowFrame f;
  SNII_RETURN_IF_ERROR(read_framed(source, &f));
  std::vector<uint8_t> holder;
  Slice plain;
  SNII_RETURN_IF_ERROR(materialize_plain(f, &holder, &plain));
  Slice dd_part = plain.subslice(0, f.dd_part_len);
  return decode_docs(dd_part, win_base, docids);
}

Status read_frq_window(ByteSource* source, uint64_t win_base, std::vector<uint32_t>* docids,
                       std::vector<uint32_t>* freqs) {
  if (source == nullptr || docids == nullptr || freqs == nullptr) {
    return Status::InvalidArgument("frq: null arg");
  }
  WindowFrame f;
  SNII_RETURN_IF_ERROR(read_framed(source, &f));
  std::vector<uint8_t> holder;
  Slice plain;
  SNII_RETURN_IF_ERROR(materialize_plain(f, &holder, &plain));
  Slice dd_part = plain.subslice(0, f.dd_part_len);
  SNII_RETURN_IF_ERROR(decode_docs(dd_part, win_base, docids));
  Slice freq_part = plain.subslice(f.dd_part_len, f.uncomp_len - f.dd_part_len);
  return decode_freqs(freq_part, docids->size(), freqs);
}

}  // namespace snii::format
