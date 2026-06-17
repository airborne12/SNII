#pragma once

#include <cstddef>
#include <cstdint>

#include "snii/common/status.h"

namespace snii {

// LEB128 变长整数编码 + zigzag。out 缓冲需 ≥10 字节，返回写入字节数。
size_t varint_len(uint64_t v);
size_t encode_varint32(uint32_t v, uint8_t* out);
size_t encode_varint64(uint64_t v, uint8_t* out);

// 解码 [p, end) 区间内的 varint；成功时 *next 指向消费后的下一字节。
Status decode_varint32(const uint8_t* p, const uint8_t* end, uint32_t* v, const uint8_t** next);
Status decode_varint64(const uint8_t* p, const uint8_t* end, uint64_t* v, const uint8_t** next);

inline uint64_t zigzag_encode(int64_t v) {
  return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}
inline int64_t zigzag_decode(uint64_t v) {
  return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

}  // namespace snii
