#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"

namespace snii::format {

// 单个 DICT block 的物理定位与校验信息。与 SampledTermIndex 按 ordinal 对齐：
// SampledTermIndex[i] 的 first_term 对应 DictBlockDirectory[i]（见 design spec
// 「词典采样索引」）。读路径据此对 [offset, offset+length) 发起一次 range read。
struct BlockRef {
  uint64_t offset = 0;     // block 在容器内的绝对字节偏移
  uint64_t length = 0;     // block 字节长度
  uint32_t n_entries = 0;  // 该 block 内 DictEntry 数量
  uint8_t flags = 0;       // block 级标志位（编码/压缩等，本期透传）
  uint32_t checksum = 0;   // 该 block 自身内容的 crc32c（读后校验用）
};

// DICT block directory：block ordinal → 物理位置映射。
//
// on-disk 布局（由 SectionFramer 框定，统一 type+len+crc32c）：
//   [u8 type=kDictBlockDirectory][varint64 payload_len][payload][fixed32 crc32c]
//   payload = varint32 n_blocks
//             then n_blocks × block_ref{
//               varint64 offset, varint64 length, varint32 n_entries,
//               u8 flags, fixed32 checksum }
// section 级 crc 提供截断/损坏检出；block_ref.checksum 是各 block 自身的 crc。
class DictBlockDirectoryBuilder {
 public:
  void add(const BlockRef& ref) { refs_.push_back(ref); }

  // 编码为一个 kDictBlockDirectory framed section（自带 crc32c），追加到 sink。
  void finish(ByteSink* sink) const;

 private:
  std::vector<BlockRef> refs_;
};

// 读取并校验一个 kDictBlockDirectory framed section，提供 ordinal → BlockRef 查询。
// 解析后所有 block_ref 常驻于 reader（随 meta 进入 searcher cache）。
class DictBlockDirectoryReader {
 public:
  // 校验 section crc，反序列化全部 block_ref。
  // crc 不匹配 / 截断 / 尾部多余字节 → kCorruption；type 非本 section → kInvalidArgument。
  static Status open(Slice section, DictBlockDirectoryReader* out);

  uint32_t n_blocks() const { return static_cast<uint32_t>(refs_.size()); }

  // 取第 ordinal 个 block_ref；ordinal >= n_blocks → kNotFound。
  Status get(uint32_t ordinal, BlockRef* out) const;

 private:
  std::vector<BlockRef> refs_;
};

}  // namespace snii::format
