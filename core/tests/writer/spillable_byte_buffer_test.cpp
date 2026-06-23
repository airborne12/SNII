#include "snii/writer/spillable_byte_buffer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/io/file_writer.h"

namespace snii::writer {
namespace {

// In-RAM sink: collects everything appended so a test can compare stream_into's
// output against the exact bytes that were fed in.
class CollectWriter : public snii::io::FileWriter {
 public:
  Status append(Slice s) override {
    bytes_.insert(bytes_.end(), s.data(), s.data() + s.size());
    written_ += s.size();
    return Status::OK();
  }
  Status finalize() override { return Status::OK(); }
  uint64_t bytes_written() const override { return written_; }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
  uint64_t written_ = 0;
};

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 31 + seed) & 0xFF);
  return v;
}

// Feeds `blocks` chunks of `block` bytes through a buffer with the given cap, then
// asserts: size() == total, spilled() matches expectation, and stream_into()
// reproduces the exact concatenation (RAM-resident or read back from the temp).
void RoundTrip(uint64_t cap, size_t block, int blocks, bool expect_spill) {
  SpillableByteBuffer buf(cap, "test");
  std::vector<uint8_t> expect;
  for (int b = 0; b < blocks; ++b) {
    const auto chunk = Pattern(block, static_cast<uint8_t>(b));
    ASSERT_TRUE(buf.append(Slice(chunk)).ok());
    expect.insert(expect.end(), chunk.begin(), chunk.end());
  }
  EXPECT_EQ(buf.size(), expect.size());
  EXPECT_EQ(buf.spilled(), expect_spill);
  ASSERT_TRUE(buf.seal().ok());
  CollectWriter out;
  ASSERT_TRUE(buf.stream_into(&out).ok());
  EXPECT_EQ(out.bytes(), expect);
}

TEST(SpillableByteBuffer, StaysInRamUnderCap) {
  RoundTrip(/*cap=*/1u << 20, /*block=*/4096, /*blocks=*/4, /*expect_spill=*/false);
}

TEST(SpillableByteBuffer, SpillsOverCapAndRoundTripsByteForByte) {
  // 8 x 4 KiB = 32 KiB through an 8 KiB cap -> spills after the 2nd block.
  RoundTrip(/*cap=*/8192, /*block=*/4096, /*blocks=*/8, /*expect_spill=*/true);
}

TEST(SpillableByteBuffer, MaxCapNeverSpills) {
  RoundTrip(/*cap=*/UINT64_MAX, /*block=*/65536, /*blocks=*/8, /*expect_spill=*/false);
}

// append_move adopts the caller's vector; verify identical bytes for RAM + spill.
void RoundTripMove(uint64_t cap, size_t block, int blocks, bool expect_spill) {
  SpillableByteBuffer buf(cap, "test");
  std::vector<uint8_t> expect;
  for (int b = 0; b < blocks; ++b) {
    auto chunk = Pattern(block, static_cast<uint8_t>(b + 7));
    expect.insert(expect.end(), chunk.begin(), chunk.end());
    ASSERT_TRUE(buf.append_move(std::move(chunk)).ok());
  }
  EXPECT_EQ(buf.size(), expect.size());
  EXPECT_EQ(buf.spilled(), expect_spill);
  ASSERT_TRUE(buf.seal().ok());
  CollectWriter out;
  ASSERT_TRUE(buf.stream_into(&out).ok());
  EXPECT_EQ(out.bytes(), expect);
}

TEST(SpillableByteBuffer, MoveAppendStaysInRam) {
  RoundTripMove(/*cap=*/1u << 20, /*block=*/4096, /*blocks=*/4, /*expect_spill=*/false);
}

TEST(SpillableByteBuffer, MoveAppendSpillsAndRoundTrips) {
  RoundTripMove(/*cap=*/8192, /*block=*/4096, /*blocks=*/8, /*expect_spill=*/true);
}

TEST(SpillableByteBuffer, EmptyBufferStreamsNothing) {
  SpillableByteBuffer buf(1u << 20, "test");
  EXPECT_EQ(buf.size(), 0u);
  EXPECT_FALSE(buf.spilled());
  ASSERT_TRUE(buf.seal().ok());
  CollectWriter out;
  ASSERT_TRUE(buf.stream_into(&out).ok());
  EXPECT_TRUE(out.bytes().empty());
}

}  // namespace
}  // namespace snii::writer
