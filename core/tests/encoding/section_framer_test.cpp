#include <gtest/gtest.h>

#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/encoding/section_framer.h"

using namespace snii;

TEST(SectionFramer, RoundTrip) {
  ByteSink sink;
  const uint8_t p[] = {9, 8, 7};
  SectionFramer::write(sink, 0x42, Slice(p, 3));
  ByteSource src(sink.view());
  FramedSection sec;
  ASSERT_TRUE(SectionFramer::read(src, &sec).ok());
  EXPECT_EQ(sec.type, 0x42);
  ASSERT_EQ(sec.payload.size(), 3u);
  EXPECT_EQ(sec.payload[0], 9u);
  EXPECT_TRUE(src.eof());
}

TEST(SectionFramer, DetectsCorruption) {
  ByteSink sink;
  const uint8_t p[] = {1, 2, 3, 4};
  SectionFramer::write(sink, 1, Slice(p, 4));
  auto bytes = sink.buffer();
  bytes[3] ^= 0xFF;  // 翻转 payload 一个字节
  Slice corrupted(bytes);
  ByteSource src(corrupted);
  FramedSection sec;
  EXPECT_EQ(SectionFramer::read(src, &sec).code(), StatusCode::kCorruption);
}

TEST(SectionFramer, SkipMultiple) {
  ByteSink sink;
  const uint8_t a[] = {1};
  const uint8_t b[] = {2, 2};
  SectionFramer::write(sink, 10, Slice(a, 1));
  SectionFramer::write(sink, 11, Slice(b, 2));
  ByteSource src(sink.view());
  FramedSection s1, s2;
  ASSERT_TRUE(SectionFramer::read(src, &s1).ok());
  ASSERT_TRUE(SectionFramer::read(src, &s2).ok());
  EXPECT_EQ(s1.type, 10);
  EXPECT_EQ(s2.type, 11);
  EXPECT_TRUE(src.eof());
}

TEST(SectionFramer, EmptyPayload) {
  ByteSink sink;
  SectionFramer::write(sink, 7, Slice());
  ByteSource src(sink.view());
  FramedSection sec;
  ASSERT_TRUE(SectionFramer::read(src, &sec).ok());
  EXPECT_EQ(sec.type, 7);
  EXPECT_EQ(sec.payload.size(), 0u);
}
