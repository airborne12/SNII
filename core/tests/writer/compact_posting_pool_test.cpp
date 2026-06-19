#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "snii/writer/compact_posting_pool.h"

using snii::writer::CompactPostingPool;

namespace {

// Test helper bundling a chain's append handle, its level, and its head -- the
// same per-term state the real accumulator keeps -- so tests can append by value.
struct Chain {
  CompactPostingPool::SliceWriter w;
  uint8_t level = 0;
  uint32_t head = 0;
  void start(CompactPostingPool* pool) { head = pool->start_chain(&w, &level); }
  void put(CompactPostingPool* pool, uint8_t b) { pool->append_byte(&w, &level, b); }
};

// Reads back the whole chain into a vector for comparison.
std::vector<uint8_t> ReadChain(const CompactPostingPool& pool, uint32_t head,
                               uint32_t len) {
  std::vector<uint8_t> out;
  out.reserve(len);
  CompactPostingPool::Cursor c = pool.cursor(head, len);
  while (c.has_next()) out.push_back(c.next());
  return out;
}

}  // namespace

// A single chain shorter than one slice round-trips exactly.
TEST(CompactPostingPool, TinyChainRoundTrips) {
  CompactPostingPool pool;
  Chain ch;
  ch.start(&pool);
  const std::vector<uint8_t> data = {7, 0, 255, 42};
  for (uint8_t b : data) ch.put(&pool, b);
  EXPECT_EQ(ReadChain(pool, ch.head, data.size()), data);
  EXPECT_EQ(pool.payload_bytes(), data.size());
}

// A chain that spans many slice levels round-trips exactly (exercises forward
// pointers across several geometric slice sizes).
TEST(CompactPostingPool, MultiSliceChainRoundTrips) {
  CompactPostingPool pool;
  Chain ch;
  ch.start(&pool);
  std::vector<uint8_t> data;
  for (uint32_t i = 0; i < 5000; ++i) data.push_back(static_cast<uint8_t>(i * 31 + 7));
  for (uint8_t b : data) ch.put(&pool, b);
  EXPECT_EQ(ReadChain(pool, ch.head, data.size()), data);
  EXPECT_EQ(pool.payload_bytes(), data.size());
}

// Many INTERLEAVED chains (the real SPIMI access pattern) stay independent: a byte
// written to chain A never appears in chain B's read-back.
TEST(CompactPostingPool, InterleavedChainsIndependent) {
  CompactPostingPool pool;
  constexpr int kChains = 64;
  std::vector<Chain> chains(kChains);
  std::vector<std::vector<uint8_t>> expect(kChains);
  for (auto& ch : chains) ch.start(&pool);

  std::mt19937 rng(12345);
  // Append bytes to chains in a randomized interleaving so slices for different
  // chains land in the same blocks intermixed.
  for (int round = 0; round < 20000; ++round) {
    const int c = static_cast<int>(rng() % kChains);
    const uint8_t b = static_cast<uint8_t>(rng());
    chains[c].put(&pool, b);
    expect[c].push_back(b);
  }
  for (int i = 0; i < kChains; ++i) {
    EXPECT_EQ(ReadChain(pool, chains[i].head, static_cast<uint32_t>(expect[i].size())),
              expect[i])
        << "chain " << i;
  }
}

// MANY chains + MANY bytes force the arena across several 32 KiB block
// boundaries. This is the regression for a block-boundary bump bug: a run that
// exactly fills a block must allocate the next block before handing out the
// boundary offset, never returning an offset into a not-yet-allocated block.
TEST(CompactPostingPool, ManyChainsAcrossBlockBoundaries) {
  CompactPostingPool pool;
  constexpr int kChains = 2000;
  std::vector<Chain> chains(kChains);
  std::vector<std::vector<uint8_t>> expect(kChains);
  for (auto& ch : chains) ch.start(&pool);

  std::mt19937 rng(98765);
  for (int round = 0; round < 1'000'000; ++round) {
    const int c = static_cast<int>(rng() % kChains);
    const uint8_t b = static_cast<uint8_t>(rng());
    chains[c].put(&pool, b);
    expect[c].push_back(b);
  }
  for (int i = 0; i < kChains; ++i) {
    EXPECT_EQ(ReadChain(pool, chains[i].head, static_cast<uint32_t>(expect[i].size())),
              expect[i])
        << "chain " << i;
  }
}

// An empty chain (started but never written) reads back as zero bytes.
TEST(CompactPostingPool, EmptyChain) {
  CompactPostingPool pool;
  Chain ch;
  ch.start(&pool);
  EXPECT_TRUE(ReadChain(pool, ch.head, 0).empty());
}

// A chain that exactly fills a slice boundary (no extra byte) reads back exactly,
// without dereferencing the (still-zero) forward pointer.
TEST(CompactPostingPool, ExactSliceBoundary) {
  CompactPostingPool pool;
  Chain ch;
  ch.start(&pool);
  // Exactly fill the level-0 slice (kSliceSizes[0] payload bytes) and stop.
  std::vector<uint8_t> data;
  for (uint32_t i = 0; i < CompactPostingPool::kSliceSizes_level0(); ++i)
    data.push_back(static_cast<uint8_t>(i + 1));
  for (uint8_t b : data) ch.put(&pool, b);
  EXPECT_EQ(ReadChain(pool, ch.head, data.size()), data);
  // Now extend by one byte (forces the forward link) and re-read fully.
  ch.put(&pool, 99);
  data.push_back(99);
  EXPECT_EQ(ReadChain(pool, ch.head, data.size()), data);
}

// reset() drops all blocks; a fresh chain after reset starts clean.
TEST(CompactPostingPool, ResetClears) {
  CompactPostingPool pool;
  Chain ch;
  ch.start(&pool);
  for (int i = 0; i < 1000; ++i) ch.put(&pool, static_cast<uint8_t>(i));
  EXPECT_GT(pool.payload_bytes(), 0u);
  pool.reset();
  EXPECT_EQ(pool.payload_bytes(), 0u);
  Chain ch2;
  ch2.start(&pool);
  std::vector<uint8_t> data = {5, 6, 7};
  for (uint8_t b : data) ch2.put(&pool, b);
  EXPECT_EQ(ReadChain(pool, ch2.head, data.size()), data);
}
