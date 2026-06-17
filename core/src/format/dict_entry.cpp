#include "snii/format/dict_entry.h"

#include <algorithm>

#include "snii/common/slice.h"

namespace snii::format {

namespace {

// flags 位的纯函数装配 / 解析，避免内联 if-else 长链。
uint8_t pack_flags(const DictEntry& e) {
  uint8_t f = 0;
  if (e.kind == DictEntryKind::kInline) f |= dict_flags::kKind;
  if (e.enc == DictEntryEnc::kWindowed) f |= dict_flags::kEnc;
  if (e.has_sb) f |= dict_flags::kHasSb;
  // bit3 has_champion / bit4 offsets_ref 在 v1 恒 0。
  return f;
}

void apply_flags(uint8_t f, DictEntry* e) {
  e->kind = (f & dict_flags::kKind) ? DictEntryKind::kInline : DictEntryKind::kPodRef;
  e->enc = (f & dict_flags::kEnc) ? DictEntryEnc::kWindowed : DictEntryEnc::kSlim;
  e->has_sb = (f & dict_flags::kHasSb) != 0;
}

// term 与 prev_term 的最长公共前缀长度。
uint32_t common_prefix_len(std::string_view term, std::string_view prev) {
  uint32_t n = 0;
  const uint32_t lim = static_cast<uint32_t>(std::min(term.size(), prev.size()));
  while (n < lim && term[n] == prev[n]) ++n;
  return n;
}

bool tier_has_stats(IndexTier tier) { return tier >= IndexTier::kT2; }

// ---- 编码 entry body（不含 entry_len 与尾部 crc）----

void write_term_key(const DictEntry& e, std::string_view prev, ByteSink* sink) {
  const uint32_t prefix = common_prefix_len(e.term, prev);
  const std::string_view suffix = std::string_view(e.term).substr(prefix);
  sink->put_varint32(prefix);
  sink->put_varint32(static_cast<uint32_t>(suffix.size()));
  sink->put_bytes(Slice(suffix));
}

void write_stats(const DictEntry& e, IndexTier tier, ByteSink* sink) {
  sink->put_varint32(e.df);
  if (!tier_has_stats(tier)) return;
  sink->put_varint64(e.ttf_delta);
  sink->put_varint64(e.max_freq);
}

void write_pod_ref(const DictEntry& e, IndexTier tier, ByteSink* sink) {
  sink->put_varint64(e.frq_off_delta);
  sink->put_varint64(e.frq_len);
  if (e.enc == DictEntryEnc::kWindowed) sink->put_varint64(e.prelude_len);
  if (!tier_has_stats(tier)) return;
  sink->put_varint64(e.prx_off_delta);
  sink->put_varint64(e.prx_len);
}

void write_inline(const DictEntry& e, IndexTier tier, ByteSink* sink) {
  sink->put_varint64(static_cast<uint64_t>(e.frq_bytes.size()));
  sink->put_bytes(Slice(e.frq_bytes));
  if (!tier_has_stats(tier)) return;
  sink->put_varint64(static_cast<uint64_t>(e.prx_bytes.size()));
  sink->put_bytes(Slice(e.prx_bytes));
}

void write_body(const DictEntry& e, std::string_view prev, IndexTier tier, ByteSink* sink) {
  write_term_key(e, prev, sink);
  sink->put_u8(pack_flags(e));
  write_stats(e, tier, sink);
  if (e.kind == DictEntryKind::kInline) {
    write_inline(e, tier, sink);
  } else {
    write_pod_ref(e, tier, sink);
  }
}

// ---- 解码 entry body ----

Status read_term_key(ByteSource* src, std::string_view prev, DictEntry* out) {
  uint32_t prefix = 0;
  uint32_t suffix_len = 0;
  SNII_RETURN_IF_ERROR(src->get_varint32(&prefix));
  SNII_RETURN_IF_ERROR(src->get_varint32(&suffix_len));
  if (prefix > prev.size()) {
    return Status::Corruption("dict_entry: prefix_len 超过 prev_term 长度");
  }
  Slice suffix;
  SNII_RETURN_IF_ERROR(src->get_bytes(suffix_len, &suffix));
  out->term.assign(prev.substr(0, prefix));
  out->term.append(reinterpret_cast<const char*>(suffix.data()), suffix.size());
  return Status::OK();
}

Status read_stats(ByteSource* src, IndexTier tier, DictEntry* out) {
  SNII_RETURN_IF_ERROR(src->get_varint32(&out->df));
  if (!tier_has_stats(tier)) return Status::OK();
  SNII_RETURN_IF_ERROR(src->get_varint64(&out->ttf_delta));
  SNII_RETURN_IF_ERROR(src->get_varint64(&out->max_freq));
  return Status::OK();
}

Status read_pod_ref(ByteSource* src, IndexTier tier, DictEntry* out) {
  SNII_RETURN_IF_ERROR(src->get_varint64(&out->frq_off_delta));
  SNII_RETURN_IF_ERROR(src->get_varint64(&out->frq_len));
  if (out->enc == DictEntryEnc::kWindowed) {
    SNII_RETURN_IF_ERROR(src->get_varint64(&out->prelude_len));
  }
  if (!tier_has_stats(tier)) return Status::OK();
  SNII_RETURN_IF_ERROR(src->get_varint64(&out->prx_off_delta));
  SNII_RETURN_IF_ERROR(src->get_varint64(&out->prx_len));
  return Status::OK();
}

Status read_byte_blob(ByteSource* src, std::vector<uint8_t>* out) {
  uint64_t len = 0;
  SNII_RETURN_IF_ERROR(src->get_varint64(&len));
  Slice bytes;
  SNII_RETURN_IF_ERROR(src->get_bytes(static_cast<size_t>(len), &bytes));
  out->assign(bytes.data(), bytes.data() + bytes.size());
  return Status::OK();
}

Status read_inline(ByteSource* src, IndexTier tier, DictEntry* out) {
  SNII_RETURN_IF_ERROR(read_byte_blob(src, &out->frq_bytes));
  if (!tier_has_stats(tier)) return Status::OK();
  SNII_RETURN_IF_ERROR(read_byte_blob(src, &out->prx_bytes));
  return Status::OK();
}

Status read_locator(ByteSource* src, IndexTier tier, DictEntry* out) {
  if (out->kind == DictEntryKind::kInline) return read_inline(src, tier, out);
  return read_pod_ref(src, tier, out);
}

// 读取 entry_len（= body 长度）并校验 src 剩余足够。
Status read_entry_len(ByteSource* src, uint64_t* total) {
  SNII_RETURN_IF_ERROR(src->get_varint64(total));
  if (*total > src->remaining()) {
    return Status::Corruption("dict_entry: entry_len 越界");
  }
  return Status::OK();
}

}  // namespace

Status encode_dict_entry(const DictEntry& entry, std::string_view prev_term,
                         IndexTier tier, ByteSink* sink) {
  if (sink == nullptr) return Status::InvalidArgument("dict_entry: sink 为空");

  // 先把 body 序列化到临时缓冲，得知精确长度后写 entry_len + body。
  // crc 校验在 DICT block 级统一进行（覆盖 block header + 全部 entries + 锚点表），
  // entry 级不重复 crc，以保持 slim/inline 低频 term 的极致紧凑（规格 §词典块/§词典项）。
  ByteSink body;
  write_body(entry, prev_term, tier, &body);
  sink->put_varint64(static_cast<uint64_t>(body.size()));
  sink->put_bytes(body.view());
  return Status::OK();
}

Status decode_dict_entry(ByteSource* src, std::string_view prev_term,
                         IndexTier tier, DictEntry* out) {
  if (src == nullptr || out == nullptr) {
    return Status::InvalidArgument("dict_entry: src / out 为空");
  }
  *out = DictEntry{};

  uint64_t total = 0;
  SNII_RETURN_IF_ERROR(read_entry_len(src, &total));
  const size_t body_start = src->position();

  SNII_RETURN_IF_ERROR(read_term_key(src, prev_term, out));
  uint8_t flags = 0;
  SNII_RETURN_IF_ERROR(src->get_u8(&flags));
  apply_flags(flags, out);
  SNII_RETURN_IF_ERROR(read_stats(src, tier, out));
  SNII_RETURN_IF_ERROR(read_locator(src, tier, out));

  // body 必须恰好消费 entry_len 字节，否则结构与 tier 不一致。
  const size_t consumed = src->position() - body_start;
  if (consumed != static_cast<size_t>(total)) {
    return Status::Corruption("dict_entry: body 长度与 entry_len 不一致");
  }
  return Status::OK();
}

Status skip_dict_entry(ByteSource* src) {
  if (src == nullptr) return Status::InvalidArgument("dict_entry: src 为空");
  uint64_t total = 0;
  SNII_RETURN_IF_ERROR(read_entry_len(src, &total));
  Slice unused;
  return src->get_bytes(static_cast<size_t>(total), &unused);
}

}  // namespace snii::format
