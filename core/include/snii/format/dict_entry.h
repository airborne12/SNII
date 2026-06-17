#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/format_constants.h"

// DictEntry —— 词典项的 on-disk 编解码。
//
// 字节布局（详见 docs/design/SNII-design-spec.source.md「词典项」章节）：
//   entry_len   varint   # entry body 的字节长度，供 reader 跳过未知扩展或快速跳过 entry
//   --- 以下为 entry body，被 entry_len 覆盖 ---
//   prefix_len  varint   # 与 prev_term 共享前缀长度
//   suffix_len  varint   # suffix 字节数
//   suffix      u8[]     # 与 prev_term 不同的后缀
//   flags       u8       # bit0 kind / bit1 enc / bit2 has_sb / bit3 has_champion(=0) / bit4 offsets_ref(=0)
//   df          varint
//   ttf_delta   varint   # 仅 tier>=T2
//   max_freq    varint   # 仅 tier>=T2
//   locator:
//     pod_ref: frq_off_delta varint, frq_len varint,
//              [prelude_len varint 当 enc=windowed],
//              [prx_off_delta varint, prx_len varint 当 tier>=T2]
//     inline:  frq_len varint, frq_bytes u8[],
//              [prx_len varint, prx_bytes u8[] 当 tier>=T2]
//   --- entry body 结束 ---
//
// crc 校验在 DICT block 级统一进行（覆盖 block header + 全部 entries + 锚点偏移表），
// entry 级不重复 crc，以保持 slim/inline 低频 term 的紧凑（规格 §词典块 line 330/348）。
// tier 与 positions 能力由 per-index meta 提供（不在 entry 内重复存储）：
// tier>=T2 时写出 ttf_delta / max_freq 及 .prx locator/bytes。
namespace snii::format {

// 词典项：内联 / 外部引用两态，自描述长度，支持块内前缀压缩。
struct DictEntry {
  // term key（前缀压缩在编解码时按 prev_term 完成，此处保存完整 term）。
  std::string term;

  // flags。
  DictEntryKind kind = DictEntryKind::kPodRef;
  DictEntryEnc enc = DictEntryEnc::kSlim;
  bool has_sb = false;

  // term stats。
  uint32_t df = 0;
  uint64_t ttf_delta = 0;  // 仅 tier>=T2
  uint64_t max_freq = 0;   // 仅 tier>=T2

  // pod_ref locator。
  uint64_t frq_off_delta = 0;
  uint64_t frq_len = 0;
  uint64_t prelude_len = 0;   // 仅 enc=windowed
  uint64_t prx_off_delta = 0; // 仅 tier>=T2
  uint64_t prx_len = 0;       // 仅 tier>=T2

  // inline payload。
  std::vector<uint8_t> frq_bytes;
  std::vector<uint8_t> prx_bytes;  // 仅 tier>=T2
};

// 将 entry 按上述布局写入 sink（追加），term 相对 prev_term 做前缀压缩。
// tier 决定可选字段是否写出。
Status encode_dict_entry(const DictEntry& entry, std::string_view prev_term,
                         IndexTier tier, ByteSink* sink);

// 从 src 当前位置解码一个 entry，term 由 prev_term + suffix 重建。
// 校验尾部 crc；越界 / crc 不符 / prefix_len 非法均返回 Corruption。
Status decode_dict_entry(ByteSource* src, std::string_view prev_term,
                         IndexTier tier, DictEntry* out);

// 仅靠 entry_len 跳过一个 entry（不解析内部字段，也不校验 crc）。
Status skip_dict_entry(ByteSource* src);

}  // namespace snii::format
