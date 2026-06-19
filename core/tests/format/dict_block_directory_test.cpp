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
  EXPECT_EQ(a.uncomp_len, b.uncomp_len);
}

}  // namespace

// A zstd-compressed block ref carries uncomp_len; a raw ref does not. Both
// round-trip exactly, and the raw ref's directory bytes stay v1-compact (no
// trailing uncomp_len varint when the kZstd flag is clear).
TEST(DictBlockDirectory, ZstdRefCarriesUncompLen) {
  std::vector<BlockRef> refs = {
      {0, 40000, 250, block_ref_flags::kZstd, 0xABCDEF01u, 65536},  // compressed
      {40000, 4096, 64, 0, 0x11223344u, 0},                         // raw
  };
  auto bytes = Build(refs);
  DictBlockDirectoryReader reader;
  ASSERT_TRUE(DictBlockDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.n_blocks(), 2u);
  BlockRef z{}, r{};
  ASSERT_TRUE(reader.get(0, &z).ok());
  ASSERT_TRUE(reader.get(1, &r).ok());
  ExpectRefEq(z, refs[0]);
  ExpectRefEq(r, refs[1]);
  EXPECT_EQ(z.uncomp_len, 65536u);
  EXPECT_TRUE((z.flags & block_ref_flags::kZstd) != 0);
  EXPECT_EQ(r.uncomp_len, 0u);
  EXPECT_FALSE((r.flags & block_ref_flags::kZstd) != 0);
}

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

// An attacker-inflated n_blocks count must hit the reserve-bomb guard in
// decode_payload BEFORE the vector reserve, returning Corruption rather than
// attempting a multi-gigabyte allocation. The payload is framed through
// SectionFramer::write so the section crc is VALID over the crafted bytes:
// this proves the inner n_blocks-vs-capacity check fires, not an outer crc flip.
TEST(DictBlockDirectory, InflatedNBlocksHitsReserveGuard) {
  ByteSink payload;
  payload.put_varint32(0xFFFFFFFFu);  // n_blocks = ~4.29 billion (impossible)
  // Only a few real bytes follow; capacity is nowhere near 4.29B refs.
  payload.put_u8(0x00);
  payload.put_u8(0x00);
  payload.put_u8(0x00);
  payload.put_u8(0x00);
  ByteSink sink;
  SectionFramer::write(sink,
                       static_cast<uint8_t>(SectionType::kDictBlockDirectory),
                       payload.view());
  DictBlockDirectoryReader reader;
  // remaining()/kMinRefBytes == 4/8 == 0, so n_blocks > 0 trips the guard.
  EXPECT_EQ(DictBlockDirectoryReader::open(Slice(sink.buffer()), &reader).code(),
            StatusCode::kCorruption);
}

// A kZstd ref whose trailing uncomp_len varint is missing (truncated) must be
// rejected as Corruption: decode_ref reads offset/length/n_entries/flags/checksum
// fine, sees the kZstd flag, then overruns when it tries to read uncomp_len.
// n_blocks == 1 with an ~8-byte ref body passes the reserve guard (1 > 8/8 is
// false), so the corruption surfaces from the inner uncomp_len read -- not the
// capacity cap and not the section crc (the frame is crc-valid by construction).
TEST(DictBlockDirectory, ZstdRefTruncatedUncompLenRejected) {
  ByteSink payload;
  payload.put_varint32(1);                       // n_blocks = 1
  payload.put_varint64(0);                        // offset (1 byte)
  payload.put_varint64(10);                       // length (1 byte)
  payload.put_varint32(1);                        // n_entries (1 byte)
  payload.put_u8(block_ref_flags::kZstd);         // flags: kZstd set
  payload.put_fixed32(0xDEADBEEFu);               // checksum (4 bytes)
  // INTENTIONALLY OMIT the trailing varint64 uncomp_len -> decode_ref overruns.
  ByteSink sink;
  SectionFramer::write(sink,
                       static_cast<uint8_t>(SectionType::kDictBlockDirectory),
                       payload.view());
  DictBlockDirectoryReader reader;
  EXPECT_EQ(DictBlockDirectoryReader::open(Slice(sink.buffer()), &reader).code(),
            StatusCode::kCorruption);
}

// A kZstd ref round-trips through the builder/reader preserving uncomp_len
// exactly, including a large 64-bit value, proving the trailing varint is both
// written and read back on the kZstd path.
TEST(DictBlockDirectory, ZstdRefRoundTripPreservesUncompLen) {
  const uint64_t kUncomp = (1ull << 40) + 1234567u;  // large, multi-byte varint
  std::vector<BlockRef> refs = {
      {1024, 999, 7, block_ref_flags::kZstd, 0x0BADF00Du, kUncomp},
  };
  auto bytes = Build(refs);
  DictBlockDirectoryReader reader;
  ASSERT_TRUE(DictBlockDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.n_blocks(), 1u);
  BlockRef out{};
  ASSERT_TRUE(reader.get(0, &out).ok());
  ExpectRefEq(out, refs[0]);
  EXPECT_EQ(out.uncomp_len, kUncomp);
  EXPECT_TRUE((out.flags & block_ref_flags::kZstd) != 0);
}
