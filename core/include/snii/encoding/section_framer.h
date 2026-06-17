#pragma once

#include <cstdint>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"

namespace snii {

// 一个被框定的 section：类型 + 负载视图。
struct FramedSection {
  uint8_t type = 0;
  Slice payload;
};

// 统一 section 框定：[u8 type][varint64 len][payload][fixed32 crc32c(type+len+payload)]。
// 全格式 section 复用此封装/校验，杜绝各自手拼。
// 未知 optional section 由调用方按 type dispatch；read 仍校验 crc 并跳过其 payload。
class SectionFramer {
 public:
  static void write(ByteSink& sink, uint8_t section_type, Slice payload);
  static Status read(ByteSource& src, FramedSection* out);
};

}  // namespace snii
