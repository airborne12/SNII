#include "snii/encoding/crc32c.h"

#include <array>

namespace snii {
namespace {

// 反射后的 Castagnoli 多项式。
constexpr uint32_t kPoly = 0x82F63B78u;

std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}

const std::array<uint32_t, 256> kTable = make_table();

}  // namespace

uint32_t crc32c_extend(uint32_t crc, Slice data) {
  crc = ~crc;
  for (size_t i = 0; i < data.size(); ++i) {
    crc = kTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace snii
