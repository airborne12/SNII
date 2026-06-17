#include "snii/format/prx_pod.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/format_constants.h"

using snii::ByteSink;
using snii::ByteSource;
using snii::Slice;
using snii::Status;
using snii::StatusCode;
using snii::format::build_prx_window;
using snii::format::read_prx_window;

namespace {

using PerDoc = std::vector<std::vector<uint32_t>>;

// 用与生产路径一致的 raw 阈值之上/之下构造数据；这里给出一个稳定可控的 round-trip 辅助。
Status RoundTrip(const PerDoc& in, int level, PerDoc* out) {
  ByteSink sink;
  SNII_RETURN_IF_ERROR(build_prx_window(in, level, &sink));
  ByteSource src(sink.view());
  SNII_RETURN_IF_ERROR(read_prx_window(&src, out));
  return Status::OK();
}

}  // namespace

// 单 doc 升序 position：delta 编码后 round-trip 必须无损。
TEST(PrxPod, SingleDocRoundTrip) {
  PerDoc in = {{3, 7, 7, 10, 100}};  // 含重复 position（delta=0）
  PerDoc out;
  ASSERT_TRUE(RoundTrip(in, -1, &out).ok());
  EXPECT_EQ(out, in);
}

// 多 doc，每 doc 内 position 升序、跨 doc 不共享基准。
TEST(PrxPod, MultiDocRoundTrip) {
  PerDoc in = {{0, 5, 12}, {1}, {2, 2, 9, 9, 40}, {7, 8, 9, 10}};
  PerDoc out;
  ASSERT_TRUE(RoundTrip(in, -1, &out).ok());
  EXPECT_EQ(out, in);
}

// 空位置：既支持 0 个 doc，也支持 doc 内 0 个 position。
TEST(PrxPod, EmptyPositions) {
  PerDoc empty_window;  // 0 个 doc
  PerDoc out1;
  ASSERT_TRUE(RoundTrip(empty_window, -1, &out1).ok());
  EXPECT_EQ(out1, empty_window);

  PerDoc with_empty_docs = {{}, {3}, {}, {}, {1, 2}};  // 含空 doc
  PerDoc out2;
  ASSERT_TRUE(RoundTrip(with_empty_docs, -1, &out2).ok());
  EXPECT_EQ(out2, with_empty_docs);
}

// 小窗口（level<0 自动）应走 raw：codec 字节 == kRaw，且无 comp_len。
TEST(PrxPod, SmallWindowUsesRaw) {
  PerDoc in = {{1, 2, 3}, {4, 5}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  ByteSource src(sink.view());
  uint8_t codec = 0xFF;
  ASSERT_TRUE(src.get_u8(&codec).ok());
  EXPECT_EQ(codec, static_cast<uint8_t>(snii::format::PrxCodec::kRaw));
}

// 大窗口（高度可压缩）自动触发 zstd，且总字节小于强制 raw 编码。
TEST(PrxPod, LargeWindowTriggersZstdAndIsSmaller) {
  PerDoc in;
  in.reserve(64);
  for (int d = 0; d < 64; ++d) {
    std::vector<uint32_t> doc;
    uint32_t p = 0;
    for (int i = 0; i < 256; ++i) {
      p += 1;  // 全 1 delta：高度可压缩
      doc.push_back(p);
    }
    in.push_back(std::move(doc));
  }

  ByteSink auto_sink;
  ASSERT_TRUE(build_prx_window(in, -1, &auto_sink).ok());
  ByteSource probe(auto_sink.view());
  uint8_t codec = 0xFF;
  ASSERT_TRUE(probe.get_u8(&codec).ok());
  EXPECT_EQ(codec, static_cast<uint8_t>(snii::format::PrxCodec::kZstd));

  // 强制 raw（level 不可能为负但用一个不触发压缩的非常大阈值不可行；改为直接对照 raw 路径）。
  ByteSink raw_sink;
  ASSERT_TRUE(build_prx_window(in, /*level=*/0, &raw_sink).ok());
  ByteSource raw_probe(raw_sink.view());
  uint8_t raw_codec = 0xFF;
  ASSERT_TRUE(raw_probe.get_u8(&raw_codec).ok());
  EXPECT_EQ(raw_codec, static_cast<uint8_t>(snii::format::PrxCodec::kRaw));

  EXPECT_LT(auto_sink.size(), raw_sink.size());

  // 压缩路径仍可无损还原。
  PerDoc out;
  ByteSource z(auto_sink.view());
  ASSERT_TRUE(read_prx_window(&z, &out).ok());
  EXPECT_EQ(out, in);
}

// level=0 显式强制 raw（不压缩），便于测试与超大单 doc 退化路径。
TEST(PrxPod, ExplicitRawLevelRoundTrip) {
  PerDoc in = {{10, 20, 30}, {40}};
  PerDoc out;
  ASSERT_TRUE(RoundTrip(in, /*level=*/0, &out).ok());
  EXPECT_EQ(out, in);
}

// crc 损坏可检出：翻转 payload 中任意一字节后 read 返回 Corruption。
TEST(PrxPod, CrcCorruptionDetected) {
  PerDoc in = {{1, 2, 3, 4, 5}, {6, 7, 8}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  std::vector<uint8_t> bytes = sink.buffer();
  ASSERT_GT(bytes.size(), 1u);
  bytes.back() ^= 0xFF;  // 破坏 payload 末字节

  Slice corrupt(bytes);
  ByteSource src(corrupt);
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// codec 字节损坏（非法 codec 值）也应被拒绝。
TEST(PrxPod, InvalidCodecRejected) {
  PerDoc in = {{1, 2}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  std::vector<uint8_t> bytes = sink.buffer();
  bytes[0] = 0x7F;  // 非法 codec（bit0-5 超出已知值）

  Slice corrupt(bytes);
  ByteSource src(corrupt);
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_FALSE(s.ok());
}

// 截断输入（payload 不足）应返回错误而非崩溃。
TEST(PrxPod, TruncatedInputRejected) {
  PerDoc in = {{1, 2, 3, 4, 5, 6, 7, 8}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  std::vector<uint8_t> bytes = sink.buffer();
  bytes.resize(bytes.size() / 2);  // 截断一半

  Slice truncated(bytes);
  ByteSource src(truncated);
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_FALSE(s.ok());
}

// 防 DoS：被损坏成超大值的 uncomp_len 必须在分配/解压前被拒绝。
TEST(PrxPod, OversizedUncompLenRejected) {
  ByteSink sink;
  sink.put_u8(static_cast<uint8_t>(snii::format::PrxCodec::kZstd));
  sink.put_varint32(300u * 1024 * 1024);  // > 256MiB 窗口上限
  // 后续 comp_len/payload/crc 不必构造——cap 检查在读 uncomp_len 后立即触发。
  ByteSource src(sink.view());
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}
