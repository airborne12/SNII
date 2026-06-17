#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/format/format_constants.h"

// SampledTermIndex —— 把查询 term 定位到候选 DICT block 的常驻元数据。
//
// 采样粒度是 DICT block（而非固定 term 数）：writer 每生成一个 DICT block，
// 就把该 block 的 first_term 写入本索引。规模与 block 数成正比。读取时随
// SniiLogicalIndexReader 进入 searcher cache。详见 design spec「词典采样索引」。
//
// on-disk 布局（由 SectionFramer 框定，统一 type+len+crc32c）：
//   [u8 type=kSampledTermIndex][varint64 payload_len][payload][fixed32 crc32c]
//   payload =
//     n_blocks       varint32
//     min_term        len(varint32) + bytes        # == sample_terms[0]，n_blocks=0 时省略
//     max_term        len(varint32) + bytes        # == sample_terms[n-1]，n_blocks=0 时省略
//     sample_terms[n_blocks]:                       # 各 block 的 first_term，升序
//       prefix_len   varint32                       # 与上一 sample_term 共享前缀长度
//       suffix_len   varint32
//       suffix       u8[suffix_len]
//
// term 字节按无符号字节序比较（UTF-8 friendly，binary-safe）。前缀压缩复用
// 与 DictEntry 一致的 prefix/suffix 原语，禁止重写。
namespace snii::format {

// 构建器：按 block ordinal 顺序追加各 DICT block 的 first_term（必须严格升序），
// finish 时一次性序列化为一个 kSampledTermIndex framed section。
class SampledTermIndexBuilder {
 public:
  // 追加下一个 DICT block 的 first_term。调用顺序即 block ordinal 顺序。
  void add_block_first_term(std::string_view first_term);

  // 序列化（追加到 sink）。空集合（无 block）也合法，n_blocks=0。
  void finish(ByteSink* sink);

 private:
  std::vector<std::string> first_terms_;
};

// 读取器：open 时校验 crc 并物化全部 sample_terms，之后 locate 为纯内存二分。
class SampledTermIndexReader {
 public:
  SampledTermIndexReader() = default;

  // 解析一个 kSampledTermIndex framed section。
  // crc 不符 / 截断 / 字段越界 → kCorruption；type 非 kSampledTermIndex → kInvalidArgument。
  static Status open(Slice section, SampledTermIndexReader* out);

  // 二分定位：返回最后一个 sample_term <= target 的 block ordinal。
  //   target < min_term 或 target > max_term（含空索引）→ *maybe_present=false（越界，term 一定不存在）。
  //   否则 *maybe_present=true，*block_ordinal 为命中 block 的序号。
  Status locate(std::string_view target, bool* maybe_present,
                uint32_t* block_ordinal) const;

  uint32_t n_blocks() const { return static_cast<uint32_t>(sample_terms_.size()); }

 private:
  std::vector<std::string> sample_terms_;
};

}  // namespace snii::format
