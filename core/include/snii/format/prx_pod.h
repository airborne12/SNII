#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"

// .prx 位置窗口（PrxPod）：保存一个窗口内若干 doc 的 term 位置信息。
//
// 单窗口 on-disk 字节布局（见 docs/design SNII「prx 设计」）：
//   u8   codec        # PrxCodec: 0=raw / 1=zstd（bit7 cont-reserved，本期忽略）
//   VInt uncomp_len   # 解压后 payload 长度，raw 也写
//   VInt comp_len     # 仅 codec != raw 时存在
//   u32  crc32c       # 覆盖 header（codec..comp_len）+ payload
//   bytes payload     # codec==raw 即明文；codec==zstd 为压缩字节
//
// 解压后 payload（自描述，使 reader 能重建 per-doc 边界）：
//   VInt doc_count
//   per doc: VInt pos_count, 然后 pos_count 个 position delta（VInt）
//   doc 内 position 升序，存 delta（首个为绝对值，其后为相邻差）。
//
// 多字节定长字段小端；变长整数复用 snii/encoding/varint。窗口尾部 crc32c 校验，损坏可检出。
namespace snii::format {

// 构建一个 .prx 窗口并追加到 sink。
// per_doc_positions[d] 为第 d 个 doc 在本窗口内的 position 列表，要求升序（可含重复）。
// zstd_level_or_negative_for_auto:
//   <0  → 自动：payload 足够大时用 ZSTD（默认 level），否则 raw。
//   0   → 强制 raw（不压缩）。
//   >0  → 强制 ZSTD，使用该 level。
// 非升序的 doc 内 position 返回 InvalidArgument。
Status build_prx_window(const std::vector<std::vector<uint32_t>>& per_doc_positions,
                        int zstd_level_or_negative_for_auto, ByteSink* sink);

// 从 source 读取并校验一个 .prx 窗口，重建 per-doc position 列表。
// crc 不匹配 / codec 非法 / 截断 / 解压失败均返回非 OK Status。
Status read_prx_window(ByteSource* source,
                       std::vector<std::vector<uint32_t>>* per_doc_positions);

}  // namespace snii::format
