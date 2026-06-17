#include "snii/format/dict_block.h"

#include <algorithm>

#include "snii/encoding/byte_source.h"
#include "snii/encoding/crc32c.h"
#include "snii/encoding/varint.h"

namespace snii::format {

namespace {

constexpr size_t kFooterBytes = sizeof(uint32_t);     // 尾部 crc32c
constexpr size_t kNAnchorsBytes = sizeof(uint32_t);   // n_anchors u32
constexpr size_t kAnchorOffBytes = sizeof(uint32_t);  // 每个锚点偏移 u32

// 估算一个 entry 编码后的上界字节数（不实际编码，供 estimated_bytes 用）。
// 取各变长字段的最大 varint 宽度 + payload 字节，保证为上界。
size_t estimate_entry_bytes(const DictEntry& e) {
  size_t body = 0;
  body += varint_len(static_cast<uint32_t>(e.term.size()));  // prefix_len 上界
  body += varint_len(static_cast<uint32_t>(e.term.size()));  // suffix_len 上界
  body += e.term.size();                                     // suffix bytes 上界
  body += 1;                                                 // flags
  body += 10;                                                // df + ttf + max_freq 上界
  body += 10;                                                // ttf_delta
  body += 10;                                                // max_freq
  if (e.kind == DictEntryKind::kInline) {
    body += 10 + e.frq_bytes.size();
    body += 10 + e.prx_bytes.size();
  } else {
    body += 10 * 5;  // frq_off/frq_len/prelude/prx_off/prx_len 上界
  }
  return varint_len(static_cast<uint64_t>(body)) + body;  // entry_len + body
}

}  // namespace

// ---- DictBlockBuilder ----

DictBlockBuilder::DictBlockBuilder(IndexTier tier, bool has_positions,
                                   uint64_t frq_base, uint64_t prx_base,
                                   uint32_t anchor_interval)
    : tier_(tier),
      has_positions_(has_positions),
      frq_base_(frq_base),
      prx_base_(prx_base),
      anchor_interval_(anchor_interval == 0 ? 1 : anchor_interval) {}

void DictBlockBuilder::add_entry(const DictEntry& entry) {
  if (is_anchor(n_entries_)) ++n_anchors_;
  entries_est_ += estimate_entry_bytes(entry);
  entries_.push_back(entry);
  prev_term_ = entry.term;
  ++n_entries_;
}

size_t DictBlockBuilder::estimated_bytes() const {
  size_t header = varint_len(static_cast<uint64_t>(n_entries_)) + 2;  // +ver +flags
  header += varint_len(frq_base_);
  if (has_positions_) header += varint_len(prx_base_);
  const size_t anchors = n_anchors_ * kAnchorOffBytes + kNAnchorsBytes;
  return header + entries_est_ + anchors + kFooterBytes;
}

void DictBlockBuilder::finish(ByteSink* sink) const {
  ByteSink body;  // header + entries + anchor_offsets + n_anchors（crc 覆盖区）

  // header。
  body.put_varint64(static_cast<uint64_t>(n_entries_));
  body.put_u8(kDictBlockFormatVer);
  body.put_u8(has_positions_ ? dict_block_flags::kHasPositions : 0u);
  body.put_varint64(frq_base_);
  if (has_positions_) body.put_varint64(prx_base_);

  // entries：锚点 entry 用 prev_term=""，并记录其块内字节偏移。
  std::vector<uint32_t> anchor_offsets;
  anchor_offsets.reserve(n_anchors_);
  std::string prev;
  for (uint32_t i = 0; i < n_entries_; ++i) {
    const bool anchor = is_anchor(i);
    if (anchor) {
      anchor_offsets.push_back(static_cast<uint32_t>(body.size()));
    }
    const std::string_view prev_term = anchor ? std::string_view{} : std::string_view(prev);
    encode_dict_entry(entries_[i], prev_term, tier_, &body);
    prev = entries_[i].term;
  }

  // anchor_offsets[] + n_anchors。
  for (uint32_t off : anchor_offsets) body.put_fixed32(off);
  body.put_fixed32(static_cast<uint32_t>(anchor_offsets.size()));

  // 整块（含 crc footer）写入 sink。
  sink->put_bytes(body.view());
  sink->put_fixed32(crc32c(body.view()));
}

// ---- DictBlockReader ----

namespace {

// 校验 block 长度足够并核对尾部 crc，返回覆盖区（不含 crc footer）的 Slice。
Status verify_crc(Slice block, Slice* covered) {
  if (block.size() < kFooterBytes + kNAnchorsBytes) {
    return Status::Corruption("dict_block: 块长度不足，无法容纳 footer");
  }
  const size_t covered_len = block.size() - kFooterBytes;
  *covered = block.subslice(0, covered_len);

  ByteSource crc_src(block.subslice(covered_len, kFooterBytes));
  uint32_t stored = 0;
  SNII_RETURN_IF_ERROR(crc_src.get_fixed32(&stored));
  if (crc32c(*covered) != stored) {
    return Status::Corruption("dict_block: crc32c 校验失败");
  }
  return Status::OK();
}

// 读取并校验 block_flags 与 has_positions 一致性。
Status check_flags(uint8_t flags, bool has_positions) {
  const bool flag_pos = (flags & dict_block_flags::kHasPositions) != 0;
  if (flag_pos != has_positions) {
    return Status::InvalidArgument("dict_block: has_positions 与 block_flags 不一致");
  }
  return Status::OK();
}

}  // namespace

Status DictBlockReader::open(Slice block, IndexTier tier, bool has_positions,
                             DictBlockReader* out) {
  if (out == nullptr) return Status::InvalidArgument("dict_block: out 为空");
  *out = DictBlockReader{};

  Slice covered;
  SNII_RETURN_IF_ERROR(verify_crc(block, &covered));
  out->block_ = covered;
  out->tier_ = tier;
  out->has_positions_ = has_positions;

  // header。
  ByteSource src(covered);
  uint64_t n_entries = 0;
  SNII_RETURN_IF_ERROR(src.get_varint64(&n_entries));
  uint8_t ver = 0;
  uint8_t flags = 0;
  SNII_RETURN_IF_ERROR(src.get_u8(&ver));
  SNII_RETURN_IF_ERROR(src.get_u8(&flags));
  if (ver != kDictBlockFormatVer) {
    return Status::Unsupported("dict_block: 不支持的 entry_format_ver");
  }
  SNII_RETURN_IF_ERROR(check_flags(flags, has_positions));
  SNII_RETURN_IF_ERROR(src.get_varint64(&out->frq_base_));
  if (has_positions) SNII_RETURN_IF_ERROR(src.get_varint64(&out->prx_base_));

  out->n_entries_ = static_cast<uint32_t>(n_entries);
  out->entries_begin_ = src.position();

  // 锚点表位于 covered 尾部：[... anchor_offsets[n] n_anchors(u32)]。
  if (covered.size() < kNAnchorsBytes) {
    return Status::Corruption("dict_block: 缺少 n_anchors");
  }
  ByteSource na_src(covered.subslice(covered.size() - kNAnchorsBytes, kNAnchorsBytes));
  uint32_t n_anchors = 0;
  SNII_RETURN_IF_ERROR(na_src.get_fixed32(&n_anchors));

  const size_t anchor_table_bytes = static_cast<size_t>(n_anchors) * kAnchorOffBytes;
  if (covered.size() < kNAnchorsBytes + anchor_table_bytes ||
      out->entries_begin_ + anchor_table_bytes + kNAnchorsBytes > covered.size()) {
    return Status::Corruption("dict_block: 锚点表越界");
  }
  const size_t anchor_table_begin = covered.size() - kNAnchorsBytes - anchor_table_bytes;

  ByteSource at_src(covered.subslice(anchor_table_begin, anchor_table_bytes));
  out->anchor_offsets_.resize(n_anchors);
  out->anchor_terms_.resize(n_anchors);
  for (uint32_t i = 0; i < n_anchors; ++i) {
    uint32_t off = 0;
    SNII_RETURN_IF_ERROR(at_src.get_fixed32(&off));
    if (off >= anchor_table_begin) {
      return Status::Corruption("dict_block: 锚点偏移越界");
    }
    // 锚点偏移必须严格单调递增，且首锚点恰为 entries 区起点（entry 0 恒为锚点）。
    // 否则 scan_from_anchor 计算段长 seg_end-seg_begin 会按 size_t 下溢导致越界读，
    // 防御被重新盖戳 crc 的非单调偏移表（远端按需读取/缓存错位场景）。
    if (i == 0) {
      if (off != out->entries_begin_) {
        return Status::Corruption("dict_block: 首锚点偏移非 entries 起点");
      }
    } else if (off <= out->anchor_offsets_[i - 1]) {
      return Status::Corruption("dict_block: 锚点偏移非严格递增");
    }
    out->anchor_offsets_[i] = off;
    // 锚点 entry 以 prev_term="" 编码，可独立解码其 term。
    ByteSource e_src(covered.subslice(off, anchor_table_begin - off));
    DictEntry probe;
    SNII_RETURN_IF_ERROR(decode_dict_entry(&e_src, std::string_view{}, tier, &probe));
    out->anchor_terms_[i] = std::move(probe.term);
  }
  return Status::OK();
}

bool DictBlockReader::locate_anchor(std::string_view target,
                                    size_t* anchor_idx) const {
  if (anchor_terms_.empty()) return false;
  if (target < std::string_view(anchor_terms_.front())) return false;
  // 最后一个 anchor_term <= target。
  size_t lo = 0;
  size_t hi = anchor_terms_.size();  // 开区间
  while (lo + 1 < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (std::string_view(anchor_terms_[mid]) <= target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  *anchor_idx = lo;
  return true;
}

Status DictBlockReader::scan_from_anchor(size_t anchor_idx,
                                         std::string_view target, bool* found,
                                         DictEntry* out) const {
  // 该锚点段的字节范围：[anchor_offset, 下一锚点 offset 或锚点表起点)。
  const size_t seg_begin = anchor_offsets_[anchor_idx];
  const bool is_last = anchor_idx + 1 == anchor_offsets_.size();
  const size_t seg_end =
      is_last ? (block_.size() - kNAnchorsBytes -
                 anchor_offsets_.size() * kAnchorOffBytes)
              : anchor_offsets_[anchor_idx + 1];

  // 兜底：open() 已校验锚点单调，此处再防御 seg_end<seg_begin 的下溢越界读。
  if (seg_end < seg_begin || seg_end > block_.size()) {
    return Status::Corruption("dict_block: 锚点段范围非法");
  }
  ByteSource src(block_.subslice(seg_begin, seg_end - seg_begin));
  std::string prev;  // 段内首个 entry 是锚点，prev_term=""
  while (!src.eof()) {
    DictEntry e;
    SNII_RETURN_IF_ERROR(decode_dict_entry(&src, std::string_view(prev), tier_, &e));
    if (e.term == target) {
      *found = true;
      *out = std::move(e);
      return Status::OK();
    }
    if (std::string_view(e.term) > target) {
      *found = false;  // 已越过 target，段内有序故不存在
      return Status::OK();
    }
    prev = std::move(e.term);
  }
  *found = false;
  return Status::OK();
}

Status DictBlockReader::find_term(std::string_view target, bool* found,
                                  DictEntry* out) const {
  if (found == nullptr || out == nullptr) {
    return Status::InvalidArgument("dict_block: found / out 为空");
  }
  *found = false;
  size_t anchor_idx = 0;
  if (!locate_anchor(target, &anchor_idx)) return Status::OK();
  return scan_from_anchor(anchor_idx, target, found, out);
}

}  // namespace snii::format
