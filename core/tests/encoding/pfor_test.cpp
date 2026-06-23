#include <gtest/gtest.h>

#include <vector>

#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/encoding/pfor.h"

using namespace snii;

static void roundtrip(const std::vector<uint32_t>& v) {
  ByteSink sink;
  pfor_encode(v.data(), v.size(), &sink);
  ByteSource src(sink.view());
  std::vector<uint32_t> out(v.size());
  ASSERT_TRUE(pfor_decode(&src, v.size(), out.data()).ok());
  EXPECT_EQ(out, v);
}

TEST(Pfor, Uniform) { roundtrip(std::vector<uint32_t>(200, 5)); }

TEST(Pfor, Ramp) {
  std::vector<uint32_t> v;
  for (uint32_t i = 0; i < 256; ++i) v.push_back(i);
  roundtrip(v);
}

TEST(Pfor, WithOutliers) {
  std::vector<uint32_t> v(128, 3);
  v[10] = 1000000;
  v[77] = 999;
  roundtrip(v);
}

TEST(Pfor, AllZero) { roundtrip(std::vector<uint32_t>(64, 0)); }

TEST(Pfor, MaxValues) {
  std::vector<uint32_t> v(32, 0xFFFFFFFFu);
  v[0] = 0;
  roundtrip(v);
}

// Exhaustive: every bit width 0..32, capped random values, across a spread of n
// (including non-power-of-two and prime lengths) so the bit-unpacker's tail handling
// (partial trailing 64-bit word / partial byte) is exercised for each width. A
// deterministic LCG keeps it reproducible without Math.random.
TEST(Pfor, AllWidthsRandomLengths) {
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  auto next = [&rng]() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(rng >> 32);
  };
  const size_t lens[] = {1, 2, 3, 7, 8, 9, 31, 32, 33, 63, 64, 65, 127, 200, 257};
  for (int w = 0; w <= 32; ++w) {
    const uint32_t cap = (w >= 32) ? 0xFFFFFFFFu : ((1u << w) - 1u);
    for (size_t n : lens) {
      std::vector<uint32_t> v(n);
      for (size_t i = 0; i < n; ++i) v[i] = (w == 0) ? 0u : (next() & cap);
      roundtrip(v);  // byte-identical decode required for every (w, n)
    }
  }
}

// A few exceptions sprinkled into otherwise-narrow data, at boundary indices, to
// cover the exception-patch path alongside the fast unpack.
TEST(Pfor, ExceptionsAtBoundaries) {
  std::vector<uint32_t> v(130, 2);
  v[0] = 0x00ABCDEF;    // first
  v[63] = 0x12345678;   // around the 64-value word boundary
  v[64] = 0x0BADF00D;
  v[129] = 0xFFFFFFFF;  // last
  roundtrip(v);
}
