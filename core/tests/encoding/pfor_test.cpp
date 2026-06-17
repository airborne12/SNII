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
