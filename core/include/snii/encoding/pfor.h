#pragma once

#include <cstddef>
#include <cstdint>

#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"

namespace snii {

// PFOR 整数块编解码（无符号 uint32 数组）。
// 编码形态：[u8 bit_width][varint n_exceptions][bit-packed low bits][exception 表]。
// 选取使总字节最小的 bit_width；超宽值进异常表 (index_delta, full_value)。
// delta/zigzag 由上层（.frq 窗口）负责，PFOR 只处理无符号整数数组。
void pfor_encode(const uint32_t* values, size_t n, ByteSink* out);
Status pfor_decode(ByteSource* src, size_t n, uint32_t* out);

}  // namespace snii
