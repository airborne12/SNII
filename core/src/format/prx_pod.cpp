#include "snii/format/prx_pod.h"

#include <cstddef>

#include "snii/common/slice.h"
#include "snii/encoding/crc32c.h"
#include "snii/encoding/zstd_codec.h"
#include "snii/format/format_constants.h"

namespace snii::format {
namespace {

// 自动压缩阈值：payload 小于此字节数时 raw（zstd 收益不足且元数据开销相对偏大）。
inline constexpr size_t kAutoZstdMinBytes = 512;
// 自动模式默认 zstd level。
inline constexpr int kDefaultZstdLevel = 3;
// 单个 .prx 窗口解压后字节上限。防御从 S3 读到被损坏成大值的 uncomp_len：
// 在分配/解压前先 sanity check，避免 GB 级分配。窗口按 256-doc 对齐，正常远小于此。
inline constexpr uint32_t kMaxWindowUncompBytes = 256u * 1024 * 1024;

// 将 per-doc position 列表编码为自描述明文 payload（doc_count + 每 doc 的 delta 流）。
Status encode_payload(const std::vector<std::vector<uint32_t>>& per_doc, ByteSink* out) {
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

// 从明文 payload 解码出 per-doc position 列表。
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

// 决策：给定 level 与明文长度，决定是否压缩。
bool should_compress(int level, size_t plain_len) {
  if (level == 0) return false;            // 强制 raw
  if (level > 0) return true;              // 强制 zstd
  return plain_len >= kAutoZstdMinBytes;   // auto
}

// 写一个 raw 窗口：codec=raw, uncomp_len, crc(header+payload), payload。
void write_raw(Slice plain, ByteSink* sink) {
  ByteSink framed;
  framed.put_u8(static_cast<uint8_t>(PrxCodec::kRaw));
  framed.put_varint32(static_cast<uint32_t>(plain.size()));
  framed.put_bytes(plain);
  sink->put_bytes(framed.view());
  sink->put_fixed32(crc32c(framed.view()));
}

// 写一个 zstd 窗口：codec=zstd, uncomp_len, comp_len, crc(header+payload), payload。
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

// 读 header + payload，回溯校验 crc，并把 payload 视图与 uncomp_len 交回调用方。
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

Status build_prx_window(const std::vector<std::vector<uint32_t>>& per_doc_positions,
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
