#pragma once

#include <cstdint>

#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/format_constants.h"

namespace snii::format {

// per-index meta block 内的统计块。仅承载查询规划与 BM25 所需的计数统计，
// section 定位信息由 SectionRefs 单独保存（见 design spec 「Per-index meta block」）。
//
// on-disk 布局（由 SectionFramer 框定，统一 type+len+crc32c）：
//   [u8 type=kStatsBlock][varint64 payload_len][payload][fixed32 crc32c]
//   payload = varint64{ doc_count, indexed_doc_count, term_count,
//                       sum_total_term_freq, null_count }
// 字段语义见 design spec 「Scoring 统计设计」。
struct StatsBlock {
  uint64_t doc_count = 0;             // segment 级 doc 总数（含未索引/NULL）
  uint64_t indexed_doc_count = 0;     // 实际参与索引的 doc 数（avgdl 分母）
  uint64_t term_count = 0;            // 该 index 的 unique term 数
  uint64_t sum_total_term_freq = 0;   // 所有 indexed doc 的 token 总数
  uint64_t null_count = 0;            // NULL / not-indexed doc 数
};

// 编码为一个 kStatsBlock framed section（自带 crc32c 校验），追加到 sink。
void encode_stats_block(const StatsBlock& sb, ByteSink* sink);

// 从 src 读取并校验一个 kStatsBlock framed section，填充 out。
// crc 不匹配 / 截断 → kCorruption；type 非 kStatsBlock → kInvalidArgument。
Status decode_stats_block(ByteSource* src, StatsBlock* out);

}  // namespace snii::format
