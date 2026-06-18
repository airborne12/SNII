#include "snii/format/prx_pod.h"

#include <cstddef>
#include <span>

#include "snii/common/slice.h"
#include "snii/encoding/crc32c.h"
#include "snii/encoding/zstd_codec.h"
#include "snii/format/format_constants.h"

namespace snii::format {
namespace {

// Auto-compression threshold: use raw when payload is smaller than this (zstd gain is negligible and metadata overhead is relatively large).
inline constexpr size_t kAutoZstdMinBytes = 512;
// Default zstd level in auto mode.
inline constexpr int kDefaultZstdLevel = 3;
// Maximum decompressed byte size for a single .prx window. Guards against a corrupted uncomp_len read from S3 inflated to a huge value:
// sanity-check before allocating/decompressing to avoid GB-scale allocations. Windows are 256-doc aligned and normally far below this limit.
inline constexpr uint32_t kMaxWindowUncompBytes = 256u * 1024 * 1024;

// Encode per-doc position lists into a self-describing plain payload (doc_count + per-doc delta stream).
Status encode_payload(std::span<const std::vector<uint32_t>> per_doc, ByteSink* out) {
  out->put_varint32(static_cast<uint32_t>(per_doc.size()));
  for (const auto& doc : per_doc) {
    out->put_varint32(static_cast<uint32_t>(doc.size()));
    uint32_t prev = 0;
    for (size_t i = 0; i < doc.size(); ++i) {
      uint32_t pos = doc[i];
      if (i > 0 && pos < prev) {
        return Status::InvalidArgument("prx: positions within a doc must be ascending");
      }
      out->put_varint32(i == 0 ? pos : pos - prev);
      prev = pos;
    }
  }
  return Status::OK();
}

// Decode per-doc position lists from a plain payload.
Status decode_payload(Slice plain, std::vector<std::vector<uint32_t>>* out) {
  ByteSource src(plain);
  uint32_t doc_count = 0;
  SNII_RETURN_IF_ERROR(src.get_varint32(&doc_count));
  out->clear();
  out->reserve(doc_count);
  for (uint32_t d = 0; d < doc_count; ++d) {
    uint32_t pos_count = 0;
    SNII_RETURN_IF_ERROR(src.get_varint32(&pos_count));
    std::vector<uint32_t> doc;
    doc.reserve(pos_count);
    uint32_t prev = 0;
    for (uint32_t i = 0; i < pos_count; ++i) {
      uint32_t delta = 0;
      SNII_RETURN_IF_ERROR(src.get_varint32(&delta));
      prev = (i == 0) ? delta : prev + delta;
      doc.push_back(prev);
    }
    out->push_back(std::move(doc));
  }
  if (!src.eof()) return Status::Corruption("prx: trailing bytes after payload");
  return Status::OK();
}

// Decision: given level and plain length, determine whether to compress.
bool should_compress(int level, size_t plain_len) {
  if (level == 0) return false;            // force raw
  if (level > 0) return true;              // force zstd
  return plain_len >= kAutoZstdMinBytes;   // auto
}

// Write a raw window: codec=raw, uncomp_len, crc(header+payload), payload.
void write_raw(Slice plain, ByteSink* sink) {
  ByteSink framed;
  framed.put_u8(static_cast<uint8_t>(PrxCodec::kRaw));
  framed.put_varint32(static_cast<uint32_t>(plain.size()));
  framed.put_bytes(plain);
  sink->put_bytes(framed.view());
  sink->put_fixed32(crc32c(framed.view()));
}

// Write a zstd window: codec=zstd, uncomp_len, comp_len, crc(header+payload), payload.
Status write_zstd(Slice plain, int level, ByteSink* sink) {
  std::vector<uint8_t> comp;
  SNII_RETURN_IF_ERROR(zstd_compress(plain, level > 0 ? level : kDefaultZstdLevel, &comp));
  ByteSink framed;
  framed.put_u8(static_cast<uint8_t>(PrxCodec::kZstd));
  framed.put_varint32(static_cast<uint32_t>(plain.size()));
  framed.put_varint32(static_cast<uint32_t>(comp.size()));
  framed.put_bytes(Slice(comp));
  sink->put_bytes(framed.view());
  sink->put_fixed32(crc32c(framed.view()));
  return Status::OK();
}

// Read header + payload, verify crc in retrospect, and return the payload view and uncomp_len to the caller.
Status read_framed(ByteSource* src, uint8_t* codec, uint32_t* uncomp_len, Slice* payload) {
  size_t start = src->position();
  SNII_RETURN_IF_ERROR(src->get_u8(codec));
  if (*codec != static_cast<uint8_t>(PrxCodec::kRaw) &&
      *codec != static_cast<uint8_t>(PrxCodec::kZstd)) {
    return Status::Corruption("prx: unknown codec");
  }
  SNII_RETURN_IF_ERROR(src->get_varint32(uncomp_len));
  if (*uncomp_len > kMaxWindowUncompBytes) {
    return Status::Corruption("prx: uncomp_len exceeds sane window cap");
  }
  size_t payload_len = *uncomp_len;
  if (*codec == static_cast<uint8_t>(PrxCodec::kZstd)) {
    uint32_t comp_len = 0;
    SNII_RETURN_IF_ERROR(src->get_varint32(&comp_len));
    payload_len = comp_len;
  }
  SNII_RETURN_IF_ERROR(src->get_bytes(payload_len, payload));
  size_t framed_len = src->position() - start;
  uint32_t stored = 0;
  SNII_RETURN_IF_ERROR(src->get_fixed32(&stored));
  if (crc32c(src->slice_from(start, framed_len)) != stored) {
    return Status::Corruption("prx: window crc mismatch");
  }
  return Status::OK();
}

}  // namespace

Status build_prx_window(std::span<const std::vector<uint32_t>> per_doc_positions,
                        int zstd_level_or_negative_for_auto, ByteSink* sink) {
  if (sink == nullptr) return Status::InvalidArgument("prx: null sink");
  ByteSink plain;
  SNII_RETURN_IF_ERROR(encode_payload(per_doc_positions, &plain));
  Slice plain_view = plain.view();
  if (!should_compress(zstd_level_or_negative_for_auto, plain_view.size())) {
    write_raw(plain_view, sink);
    return Status::OK();
  }
  return write_zstd(plain_view, zstd_level_or_negative_for_auto, sink);
}

Status read_prx_window(ByteSource* source,
                       std::vector<std::vector<uint32_t>>* per_doc_positions) {
  if (source == nullptr || per_doc_positions == nullptr) {
    return Status::InvalidArgument("prx: null arg");
  }
  uint8_t codec = 0;
  uint32_t uncomp_len = 0;
  Slice payload;
  SNII_RETURN_IF_ERROR(read_framed(source, &codec, &uncomp_len, &payload));
  if (codec == static_cast<uint8_t>(PrxCodec::kRaw)) {
    return decode_payload(payload, per_doc_positions);
  }
  std::vector<uint8_t> plain;
  SNII_RETURN_IF_ERROR(zstd_decompress(payload, uncomp_len, &plain));
  return decode_payload(Slice(plain), per_doc_positions);
}

}  // namespace snii::format
