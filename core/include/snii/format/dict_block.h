#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"

// DICT block —— 词典块：term → 倒排数据读取计划的定位单元，也是远端按需读取、
// 缓存与 crc 校验的基本单位（详见 docs/design/SNII-design-spec.source.md「词典块」
// 与「总结词典查询流程」章节）。
//
// 字节布局（严格实现，多字节定长字段小端，变长整数 LEB128）：
//   header:
//     n_entries        varint
//     entry_format_ver u8        # = kDictBlockFormatVer
//     block_flags      u8        # bit0 = has_positions（与 reader 传入一致性校验）
//     frq_base         varint64
//     prx_base         varint64  # 仅 has_positions 时存在
//   entries[n_entries]           # 变长 DictEntry，按字典序前缀压缩
//   anchor_offsets[n_anchors]    # u32 * n_anchors，每个锚点 entry 在 block 内的字节偏移
//   n_anchors        u32
//   crc32c           u32         # 覆盖 [header .. n_anchors]，损坏可检出（唯一 crc 层）
//
// 锚点规则：每隔 anchor_interval 个 entry 强制一个「词项锚点」——该 entry 以
// prev_term="" 编码（prefix_len=0，存完整 term），其字节偏移记入 anchor_offsets；
// 非锚点 entry 用前一个 entry 的 term 做 prev_term 前缀压缩。reader 可从任一锚点
// 独立扫描，无需更早的 term，从而支持锚点二分 + 局部扫描的 exact term 查找。
namespace snii::format {

// DICT block entry_format_ver（与 DictEntry 编码版本对齐；v1 固定为 1）。
inline constexpr uint8_t kDictBlockFormatVer = 1;

// block_flags 位定义。
namespace dict_block_flags {
inline constexpr uint8_t kHasPositions = 1u << 0;  // 是否写出 prx_base / .prx 字段
// bit1-7 reserved
}  // namespace dict_block_flags

// DICT block 写入器：按字典序顺序 add_entry，内部维护 prev_term、决定锚点、
// 累积大小估算，finish 时一次性序列化 header + entries + 锚点表 + crc。
class DictBlockBuilder {
 public:
  DictBlockBuilder(IndexTier tier, bool has_positions, uint64_t frq_base,
                   uint64_t prx_base, uint32_t anchor_interval = 16);

  // 追加一个 entry（调用方保证按 term 字典序）。内部决定其是否为锚点。
  void add_entry(const DictEntry& entry);

  // 当前 block 序列化大小的上界估算（含 header + entries + 锚点表 + crc footer），
  // 供上层按 target_dict_block_bytes 决定切块。
  size_t estimated_bytes() const;

  // entry 数量。
  uint32_t n_entries() const { return n_entries_; }

  // 序列化整个 block 追加到 sink。
  void finish(ByteSink* sink) const;

 private:
  bool is_anchor(uint32_t index) const {
    return index % anchor_interval_ == 0;
  }

  IndexTier tier_;
  bool has_positions_;
  uint64_t frq_base_;
  uint64_t prx_base_;
  uint32_t anchor_interval_;

  uint32_t n_entries_ = 0;
  std::vector<DictEntry> entries_;
  std::string prev_term_;        // 上一个 entry 的 term（前缀压缩基准）
  size_t entries_est_ = 0;       // entries 区累积字节估算
  size_t n_anchors_ = 0;         // 锚点数量
};

// DICT block 读取器：open 时校验 crc 并解析 header / 锚点表，find_term 用锚点
// 二分 + 局部扫描定位 DictEntry。持有 block 字节视图（不拥有），生命周期由调用方负责。
class DictBlockReader {
 public:
  DictBlockReader() = default;

  // 解析并校验整个 block。crc 不符 / 截断 / 结构非法 → Corruption；
  // header 中 has_positions 与传入参数不一致 → InvalidArgument。
  static Status open(Slice block, IndexTier tier, bool has_positions,
                     DictBlockReader* out);

  // 锚点二分 + 局部扫描查找 target。命中 → *found=true 且填充 *out；
  // 未命中（含越界、gap）→ *found=false。结构错误 → 非 OK Status。
  Status find_term(std::string_view target, bool* found, DictEntry* out) const;

  uint64_t frq_base() const { return frq_base_; }
  uint64_t prx_base() const { return prx_base_; }
  uint32_t n_entries() const { return n_entries_; }

 private:
  // 从锚点 anchor_idx 起顺序扫描至该锚点段末尾，查找 target。
  Status scan_from_anchor(size_t anchor_idx, std::string_view target,
                          bool* found, DictEntry* out) const;

  // 找到最后一个 first_term(anchor) <= target 的锚点下标；无则返回 false。
  bool locate_anchor(std::string_view target, size_t* anchor_idx) const;

  Slice block_;                       // [header .. crc) 整块视图
  IndexTier tier_ = IndexTier::kT1;
  bool has_positions_ = false;
  uint64_t frq_base_ = 0;
  uint64_t prx_base_ = 0;
  uint32_t n_entries_ = 0;

  size_t entries_begin_ = 0;          // entries 区起始绝对偏移
  std::vector<uint32_t> anchor_offsets_;  // 各锚点 entry 的块内字节偏移
  std::vector<std::string> anchor_terms_;  // 各锚点 entry 的完整 term（供二分）
};

}  // namespace snii::format
