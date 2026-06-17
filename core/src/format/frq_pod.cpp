#include "snii/format/frq_pod.h"

#include <cstddef>

#include "snii/common/slice.h"
#include "snii/encoding/crc32c.h"
#include "snii/encoding/pfor.h"
#include "snii/encoding/zstd_codec.h"
#include "snii/format/format_constants.h"

namespace snii::format {
namespace {

// 自动压缩阈值：payload 小于此字节数时 raw（zstd 收益不足且元数据开销相对偏大）。
inline constexpr size_t kAutoZstdMinBytes = 512;
// 自动模式默认 zstd level。
inline constexpr int kDefaultZstdLevel = 3;
// 单个 .frq 窗口解压后字节上限。防御从 S3 读到被损坏成大值的 uncomp_len：
// 在分配/解压前先 sanity check，避免 GB 级分配。窗口按 256-doc 对齐，正常远小于此。
inline constexpr uint32_t kMaxWindowUncompBytes = 256u * 1024 * 1024;
// 单个 .frq 窗口最多 doc 数上限（防御被损坏的 n）。窗口基准 256，实际组合上限 2048，
// 这里取一个宽松但能拦住天文数字的上界。
inline constexpr uint32_t kMaxWindowDocs = 1u << 24;

bool is_valid_win_mode(uint8_t v) {
  return v == static_cast<uint8_t>(FrqWinMode::kRaw) ||
         v == static_cast<uint8_t>(FrqWinMode::kZstd);
}

// 将一个 uint32 数组按 256(kFrqBaseUnit) 一段写成多个 PFOR run。
// 不写 n / run 数：run 段数由总长度 n 与 kFrqBaseUnit 推导，解码方一致计算。
void encode_pfor_runs(const std::vector<uint32_t>& values, ByteSink* out) {
  size_t n = values.size();
  for (size_t off = 0; off < n; off += kFrqBaseUnit) {
    size_t run = (n - off < kFrqBaseUnit) ? (n - off) : kFrqBaseUnit;
    pfor_encode(values.data() + off, run, out);
  }
}

// 从 source 解出 n 个 uint32（按 256 一段的多个 PFOR run）。
Status decode_pfor_runs(ByteSource* src, size_t n, std::vector<uint32_t>* out) {
  out->assign(n, 0);
  for (size_t off = 0; off < n; off += kFrqBaseUnit) {
    size_t run = (n - off < kFrqBaseUnit) ? (n - off) : kFrqBaseUnit;
    SNII_RETURN_IF_ERROR(pfor_decode(src, run, out->data() + off));
  }
  return Status::OK();
}

// 校验 docids 升序、首项不低于 win_base、freq 长度匹配。
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

// 编码明文 payload：dd_part(=VInt n ++ PFOR_runs(doc_delta)) ++ freq_part(=PFOR_runs(freq))。
// 返回 dd_part 字节长度（供 header 的 dd_part_len）。
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

// 决策：给定 level 与明文长度，决定是否压缩。
bool should_compress(int level, size_t plain_len) {
  if (level == 0) return false;           // 强制 raw
  if (level > 0) return true;             // 强制 zstd
  return plain_len >= kAutoZstdMinBytes;  // auto
}

// 写一个 raw 窗口：win_mode=raw, uncomp_len, dd_part_len, crc(header+payload), payload。
void write_raw(Slice plain, size_t dd_part_len, ByteSink* sink) {
  ByteSink framed;
  framed.put_u8(static_cast<uint8_t>(FrqWinMode::kRaw));
  framed.put_varint32(static_cast<uint32_t>(plain.size()));
  framed.put_varint32(static_cast<uint32_t>(dd_part_len));
  framed.put_bytes(plain);
  sink->put_bytes(framed.view());
  sink->put_fixed32(crc32c(framed.view()));
}

// 写一个 zstd 窗口：win_mode=zstd, uncomp_len, comp_len, dd_part_len, crc(header+payload), payload。
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

// 已解析的窗口头与 payload 视图。
struct WindowFrame {
  uint8_t win_mode = 0;
  uint32_t uncomp_len = 0;
  uint32_t dd_part_len = 0;
  Slice payload;  // raw 即明文；zstd 为压缩字节
};

// 读 header + payload，回溯校验 crc，把头字段与 payload 视图交回调用方。
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

// 解 dd 段：从明文 dd_part 读 n 与 doc_delta，重建升序 docids。
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

// 解 freq 段：dd_part 之后的明文为 PFOR_runs(freq)，共 doc_count 个。
Status decode_freqs(Slice freq_part, size_t doc_count, std::vector<uint32_t>* freqs) {
  if (freq_part.empty()) {
    freqs->clear();
    return Status::OK();
  }
  ByteSource src(freq_part);
  return decode_pfor_runs(&src, doc_count, freqs);
}

// 取得明文 payload（raw 直接借视图，zstd 解压到 holder）。
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
