#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/dict_block_directory.h"
#include "snii/format/format_constants.h"

using namespace snii;
using namespace snii::format;

namespace {

// Serialize a list of block_refs using the builder and return the framed section bytes.
std::vector<uint8_t> Build(const std::vector<BlockRef>& refs) {
  DictBlockDirectoryBuilder builder;
  for (const auto& r : refs) {
    builder.add(r);
  }
  ByteSink sink;
  builder.finish(&sink);
  return sink.buffer();
}

// Assert that two BlockRef structs are equal field by field.
void ExpectRefEq(const BlockRef& a, const BlockRef& b) {
  EXPECT_EQ(a.offset, b.offset);
  EXPECT_EQ(a.length, b.length);
  EXPECT_EQ(a.n_entries, b.n_entries);
  EXPECT_EQ(a.flags, b.flags);
  EXPECT_EQ(a.checksum, b.checksum);
}

}  // namespace

TEST(DictBlockDirectory, RoundTripMultipleRefs) {
  std::vector<BlockRef> refs = {
      {0, 4096, 120, 0x01, 0xDEADBEEFu},
      {4096, 8192, 300, 0x05, 0x12345678u},
      {12288, 2048, 64, 0x00, 0xCAFEBABEu},
  };
  auto bytes = Build(refs);

  DictBlockDirectoryReader reader;
  ASSERT_TRUE(DictBlockDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.n_blocks(), 3u);
  for (uint32_t i = 0; i < refs.size(); ++i) {
    BlockRef out{};
    ASSERT_TRUE(reader.get(i, &out).ok());
    ExpectRefEq(out, refs[i]);
  }
}

TEST(DictBlockDirectory, GetOrdinalCorrectMapping) {
  std::vector<BlockRef> refs;
  for (uint32_t i = 0; i < 50; ++i) {
    refs.push_back(BlockRef{static_cast<uint64_t>(i) * 1000, 1000,
                            i + 1, static_cast<uint8_t>(i & 0xFF), i * 7u + 3u});
  }
  auto bytes = Build(refs);

  DictBlockDirectoryReader reader;
  ASSERT_TRUE(DictBlockDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.n_blocks(), 50u);
  // Sample several ordinals and verify the mapping produces no cross-slot errors.
  for (uint32_t ord : {0u, 1u, 17u, 49u}) {
    BlockRef out{};
    ASSERT_TRUE(reader.get(ord, &out).ok());
    ExpectRefEq(out, refs[ord]);
  }
}

TEST(DictBlockDirectory, OutOfRangeOrdinalRejected) {
  std::vector<BlockRef> refs = {{0, 100, 1, 0, 0xAAu}};
  auto bytes = Build(refs);

  DictBlockDirectoryReader reader;
  ASSERT_TRUE(DictBlockDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.n_blocks(), 1u);
  BlockRef out{};
  // ordinal == n_blocks is out of range
  EXPECT_EQ(reader.get(1, &out).code(), StatusCode::kNotFound);
  // far beyond range
  EXPECT_EQ(reader.get(1000, &out).code(), StatusCode::kNotFound);
}

TEST(DictBlockDirectory, EmptyDirectory) {
  std::vector<BlockRef> refs;  // 0 blocks
  auto bytes = Build(refs);

  DictBlockDirectoryReader reader;
  ASSERT_TRUE(DictBlockDirectoryReader::open(Slice(bytes), &reader).ok());
  EXPECT_EQ(reader.n_blocks(), 0u);
  BlockRef out{};
  EXPECT_EQ(reader.get(0, &out).code(), StatusCode::kNotFound);
}

TEST(DictBlockDirectory, LargeOffsetNear2Pow48) {
  const uint64_t kBig = (1ull << 48) - 1;  // near 2^48
  std::vector<BlockRef> refs = {
      {kBig, kBig - 1, 0xFFFFFFFFu, 0xFF, 0xFFFFFFFFu},
      {kBig + 12345, 1, 1, 0x02, 0u},
  };
  auto bytes = Build(refs);

  DictBlockDirectoryReader reader;
  ASSERT_TRUE(DictBlockDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.n_blocks(), 2u);
  BlockRef out0{};
  ASSERT_TRUE(reader.get(0, &out0).ok());
  ExpectRefEq(out0, refs[0]);
  BlockRef out1{};
  ASSERT_TRUE(reader.get(1, &out1).ok());
  ExpectRefEq(out1, refs[1]);
}

TEST(DictBlockDirectory, FramedAsDictBlockDirectoryType) {
  std::vector<BlockRef> refs = {{0, 10, 1, 0, 0}};
  auto bytes = Build(refs);
  ASSERT_GE(bytes.size(), 1u);
  // The first byte is the SectionFramer type and must be kDictBlockDirectory.
  EXPECT_EQ(bytes[0], static_cast<uint8_t>(SectionType::kDictBlockDirectory));
}

TEST(DictBlockDirectory, DetectsCorruption) {
  std::vector<BlockRef> refs = {
      {0, 4096, 120, 0x01, 0xDEADBEEFu},
      {4096, 8192, 300, 0x05, 0x12345678u},
  };
  auto bytes = Build(refs);
  ASSERT_GE(bytes.size(), 4u);
  // Flip one byte in the payload region (skip the type+len prefix); the section CRC must detect the corruption.
  bytes[3] ^= 0xFF;
  DictBlockDirectoryReader reader;
  EXPECT_EQ(DictBlockDirectoryReader::open(Slice(bytes), &reader).code(),
            StatusCode::kCorruption);
}

TEST(DictBlockDirectory, DetectsTruncation) {
  std::vector<BlockRef> refs = {{0, 4096, 120, 0x01, 0xDEADBEEFu}};
  auto bytes = Build(refs);
  bytes.pop_back();  // truncate the last byte
  DictBlockDirectoryReader reader;
  EXPECT_FALSE(DictBlockDirectoryReader::open(Slice(bytes), &reader).ok());
}

TEST(DictBlockDirectory, WrongSectionTypeRejected) {
  // Write a section with a type other than kDictBlockDirectory via the framer; open must reject it.
  ByteSink sink;
  const uint8_t p[] = {0, 1, 2};
  SectionFramer::write(sink, static_cast<uint8_t>(SectionType::kXFilter),
                       Slice(p, 3));
  auto bytes = sink.buffer();
  DictBlockDirectoryReader reader;
  EXPECT_EQ(DictBlockDirectoryReader::open(Slice(bytes), &reader).code(),
            StatusCode::kInvalidArgument);
}

TEST(DictBlockDirectory, TrailingBytesRejected) {
  // Extra trailing bytes at the end of the payload should be detected (n_blocks does not match actual data).
  ByteSink payload;
  payload.put_varint32(1);            // n_blocks = 1
  payload.put_varint64(0);            // offset
  payload.put_varint64(10);           // length
  payload.put_varint32(1);            // n_entries
  payload.put_u8(0);                  // flags
  payload.put_fixed32(0);            // checksum
  payload.put_u8(0xEE);               // extra trailing byte
  ByteSink sink;
  SectionFramer::write(sink,
                       static_cast<uint8_t>(SectionType::kDictBlockDirectory),
                       payload.view());
  DictBlockDirectoryReader reader;
  EXPECT_EQ(DictBlockDirectoryReader::open(Slice(sink.buffer()), &reader).code(),
            StatusCode::kCorruption);
}
