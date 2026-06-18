#include <gtest/gtest.h>

#include <vector>

#include "snii/encoding/crc32c.h"

using namespace snii;

// leveldb/RocksDB standard CRC32C(Castagnoli) test vectors.
TEST(Crc32c, KnownVectors) {
  std::vector<uint8_t> zeros(32, 0x00);
  EXPECT_EQ(crc32c(Slice(zeros)), 0x8a9136aau);
  std::vector<uint8_t> ff(32, 0xff);
  EXPECT_EQ(crc32c(Slice(ff)), 0x62a8ab43u);
  std::vector<uint8_t> ramp(32);
  for (int i = 0; i < 32; ++i) ramp[i] = static_cast<uint8_t>(i);
  EXPECT_EQ(crc32c(Slice(ramp)), 0x46dd794eu);
}

TEST(Crc32c, ExtendEqualsContiguous) {
  std::vector<uint8_t> v{1, 2, 3, 4, 5, 6, 7, 8};
  uint32_t whole = crc32c(Slice(v));
  uint32_t part = crc32c(Slice(v.data(), 4));
  part = crc32c_extend(part, Slice(v.data() + 4, 4));
  EXPECT_EQ(whole, part);
}

namespace {

// Independent byte-at-a-time CRC32C reference (bit-reflected Castagnoli). Used to
// pin the optimized slice-by-8 / hardware path to the canonical scalar result.
uint32_t crc32c_ref(const std::vector<uint8_t>& data) {
  static constexpr uint32_t kPoly = 0x82F63B78u;
  uint32_t crc = ~0u;
  for (uint8_t b : data) {
    crc ^= b;
    for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (kPoly ^ (crc >> 1)) : (crc >> 1);
  }
  return ~crc;
}

}  // namespace

// Sweep every length 0..2048: stresses the 8-byte main loop plus the 0..7 residue
// tail (and the hardware u32/u8 tails) against the scalar reference. Catches any
// slice-by-8 table or unaligned-load bug that the fixed-size vectors above miss.
TEST(Crc32c, MatchesScalarReferenceAllLengths) {
  std::vector<uint8_t> data;
  data.reserve(2048);
  uint32_t x = 0x12345678u;
  for (size_t len = 0; len <= 2048; ++len) {
    EXPECT_EQ(crc32c(Slice(data)), crc32c_ref(data)) << "len=" << len;
    // Pseudo-random next byte (xorshift) so the stream is non-trivial.
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    data.push_back(static_cast<uint8_t>(x));
  }
}

// Splitting a buffer at an arbitrary boundary and extending must equal the
// one-shot crc -- the property the windowed/region layout relies on.
TEST(Crc32c, ExtendAcrossArbitrarySplits) {
  std::vector<uint8_t> data(300);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + 1);
  const uint32_t whole = crc32c(Slice(data));
  for (size_t split : {0u, 1u, 7u, 8u, 9u, 16u, 100u, 299u, 300u}) {
    uint32_t c = crc32c(Slice(data.data(), split));
    c = crc32c_extend(c, Slice(data.data() + split, data.size() - split));
    EXPECT_EQ(c, whole) << "split=" << split;
  }
}
