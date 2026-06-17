#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/format_constants.h"
#include "snii/format/norms_pod.h"

using namespace snii;
using snii::format::NormsPodReader;
using snii::format::NormsPodWriter;

namespace {

// 用 writer 把一串 encoded_norm 写成 framed payload，再返回缓冲区。
std::vector<uint8_t> BuildPod(const std::vector<uint8_t>& norms) {
  NormsPodWriter writer;
  for (uint8_t n : norms) {
    writer.add(n);
  }
  ByteSink sink;
  writer.finish(&sink);
  return sink.buffer();
}

}  // namespace

// N 个 norm 写入后逐 doc 读回一致。
TEST(NormsPod, RoundTripValues) {
  std::vector<uint8_t> norms = {0, 1, 7, 42, 128, 200, 255};
  NormsPodWriter writer;
  for (uint8_t n : norms) {
    writer.add(n);
  }
  EXPECT_EQ(writer.count(), norms.size());

  ByteSink sink;
  writer.finish(&sink);

  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(sink.view(), &reader).ok());
  ASSERT_EQ(reader.doc_count(), norms.size());
  for (uint32_t docid = 0; docid < norms.size(); ++docid) {
    EXPECT_EQ(reader.encoded_norm(docid), norms[docid]) << "docid=" << docid;
  }
}

// 较大规模，覆盖 varint doc_count 多字节路径。
TEST(NormsPod, RoundTripLarge) {
  std::vector<uint8_t> norms;
  norms.reserve(5000);
  for (uint32_t i = 0; i < 5000; ++i) {
    norms.push_back(static_cast<uint8_t>((i * 31 + 7) & 0xFF));
  }
  auto buf = BuildPod(norms);

  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(Slice(buf), &reader).ok());
  ASSERT_EQ(reader.doc_count(), 5000u);
  for (uint32_t docid = 0; docid < 5000; ++docid) {
    EXPECT_EQ(reader.encoded_norm(docid), norms[docid]) << "docid=" << docid;
  }
}

// 空 POD：count = 0 合法，可正常 open。
TEST(NormsPod, EmptyPod) {
  NormsPodWriter writer;
  EXPECT_EQ(writer.count(), 0u);

  ByteSink sink;
  writer.finish(&sink);

  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(sink.view(), &reader).ok());
  EXPECT_EQ(reader.doc_count(), 0u);
}

// crc 损坏可检出：翻转 payload 中一个字节后 open 失败（Corruption）。
TEST(NormsPod, DetectsCorruption) {
  std::vector<uint8_t> norms = {10, 20, 30, 40, 50};
  auto buf = BuildPod(norms);
  // 翻转靠后位置（落在 payload 的 norm 字节区，而非 framer 头部）。
  buf[buf.size() - 3] ^= 0xFF;

  NormsPodReader reader;
  Status s = NormsPodReader::open(Slice(buf), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// 截断的输入应返回错误而非崩溃。
TEST(NormsPod, DetectsTruncation) {
  std::vector<uint8_t> norms = {1, 2, 3, 4, 5, 6, 7, 8};
  auto buf = BuildPod(norms);
  buf.resize(buf.size() - 4);  // 砍掉尾部 crc 区

  NormsPodReader reader;
  Status s = NormsPodReader::open(Slice(buf), &reader);
  EXPECT_FALSE(s.ok());
}

// 声明的 doc_count 与实际 payload 字节数不一致应被检出。
TEST(NormsPod, DetectsLengthMismatch) {
  // 手工构造：framer payload = [varint doc_count=4][仅 2 个 norm 字节]。
  ByteSink payload;
  payload.put_varint64(4);
  payload.put_u8(11);
  payload.put_u8(22);

  ByteSink sink;
  // 复用 framer 以保证 crc 自洽，专门触发 length-mismatch 分支。
  snii::SectionFramer::write(
      sink, static_cast<uint8_t>(snii::format::SectionType::kStatsBlock),
      payload.view());

  NormsPodReader reader;
  Status s = NormsPodReader::open(sink.view(), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

#ifndef NDEBUG
// debug 构建下越界 docid 触发断言（death test）。
TEST(NormsPodDeathTest, OutOfRangeDocidAsserts) {
  std::vector<uint8_t> norms = {3, 6, 9};
  auto buf = BuildPod(norms);
  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(Slice(buf), &reader).ok());
  EXPECT_DEATH({ (void)reader.encoded_norm(3); }, "");
}
#endif

// 受检访问：合法 docid 返回值，越界返回 InvalidArgument（Release 下也生效）。
TEST(NormsPod, TryEncodedNormChecksBounds) {
  std::vector<uint8_t> norms = {3, 6, 9};
  auto buf = BuildPod(norms);
  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(Slice(buf), &reader).ok());
  uint8_t v = 0;
  ASSERT_TRUE(reader.try_encoded_norm(1, &v).ok());
  EXPECT_EQ(v, 6u);
  Status s = reader.try_encoded_norm(3, &v);
  EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}
