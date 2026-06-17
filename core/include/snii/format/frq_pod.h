#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"

// .frq 窗口（FrqPod）：doc/freq 倒排数据，列式 + PFOR（见 docs/design SNII「frq 设计」）。
//
// 一个窗口含 n 个升序 doc（基准 unit=256，可组合 256/512/1024/2048）。win_base 由调用方
// 提供：首窗=0，非首窗=上一窗最后 docid。dd[0]=first_docid-win_base；dd[i]=docid[i]-docid[i-1]。
//
// 单窗口 on-disk 字节布局：
//   u8   win_mode      # FrqWinMode: 0=raw / 1=zstd
//   VInt uncomp_len    # 解压后 payload 字节数（raw 也写）
//   VInt comp_len      # 仅 win_mode==kZstd 时存在
//   VInt dd_part_len   # payload 内 dd 部分字节长度；bitmap-only 据此跳过 freq
//   u32  crc32c        # 覆盖 header（win_mode..dd_part_len）+ payload
//   bytes payload(解压后) = dd_part ++ freq_part
//
// payload 解压后两段（列式：先全部 doc_delta，再全部 freq）：
//   dd_part   = VInt n ++ PFOR_runs(doc_delta)   # n 为 doc 数，使窗口自描述
//   freq_part = PFOR_runs(freq)                  # has_freq=false 时为空
// PFOR run 以 256-doc 为一段（kFrqBaseUnit），不足一段写余量。
// docs-only（has_freq=false）：freq_part 为空，dd_part_len==uncomp_len。
//
// 多字节定长字段小端；变长整数复用 snii/encoding/varint；PFOR 复用 snii/encoding/pfor。
// 窗口尾部 crc32c 覆盖 header+payload，损坏可检出。
namespace snii::format {

// 构建一个 .frq 窗口并追加到 sink。
// docids_ascending：本窗口内升序 docid（可单 doc，可空窗口）。
// freqs：has_freq=true 时长度必须等于 docids；has_freq=false 时忽略（建议传空）。
// win_base：基准 docid（首窗=0，非首窗=上一窗最后 docid）；要求 docids[0] >= win_base。
// zstd_level_or_neg_for_auto:
//   <0  → 自动：payload 足够大时用 ZSTD（默认 level），否则 raw。
//   0   → 强制 raw（不压缩）。
//   >0  → 强制 ZSTD，使用该 level。
// 非升序 docid / freq 长度不匹配 / first_docid < win_base / null sink 返回 InvalidArgument。
Status build_frq_window(const std::vector<uint32_t>& docids_ascending,
                        const std::vector<uint32_t>& freqs, uint64_t win_base, bool has_freq,
                        int zstd_level_or_neg_for_auto, ByteSink* sink);

// bitmap-only 路径：只解码 dd 段重建 docids，按 dd_part_len 跳过 freq（raw/zstd 皆可）。
// crc 不匹配 / win_mode 非法 / 截断 / 解压失败均返回非 OK Status。
Status read_frq_window_docs(ByteSource* source, uint64_t win_base,
                            std::vector<uint32_t>* docids);

// scoring 路径：同时解码 docids 与 freqs。
// has_freq=false 的窗口读出的 freqs 为空。
// crc 不匹配 / win_mode 非法 / 截断 / 解压失败均返回非 OK Status。
Status read_frq_window(ByteSource* source, uint64_t win_base, std::vector<uint32_t>* docids,
                       std::vector<uint32_t>* freqs);

}  // namespace snii::format
