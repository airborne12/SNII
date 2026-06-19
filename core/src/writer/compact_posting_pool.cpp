#include "snii/writer/compact_posting_pool.h"

#include <cstring>

namespace snii::writer {

// Gentle (~1.5x) many-level payload-capacity schedule. Starting at 5 bytes with a
// slow ramp keeps the over-allocated FINAL slice small for the millions of low-df
// terms (the dominant arena-overhead source) while still reaching multi-KiB slices
// for high-df chains in a bounded number of hops (so the per-slice 4-byte forward
// pointer stays a small fraction of a large chain's bytes).
const uint32_t CompactPostingPool::kSliceSizes[kLevelCount] = {
    5, 8, 12, 18, 27, 40, 60, 90, 135, 202, 303, 455, 683, 1024, 1536, 2304};
const uint8_t CompactPostingPool::kNextLevel[kLevelCount] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15};

CompactPostingPool::CompactPostingPool() = default;

uint32_t CompactPostingPool::kSliceSizes_level0() { return kSliceSizes[0]; }

void CompactPostingPool::reset() {
  std::vector<std::vector<uint8_t>>().swap(blocks_);
  next_offset_ = 0;
  payload_bytes_ = 0;
}

uint32_t CompactPostingPool::alloc_run(uint32_t bytes) {
  const uint32_t in_block = next_offset_ & kBlockMask;
  // A fresh block is needed when (a) there is no tail block yet, (b) the run does
  // not fit in the current tail block's remaining space, or (c) next_offset_ sits
  // exactly on a block boundary whose block has not been allocated (a previous run
  // that exactly filled the tail leaves next_offset_ == blocks_.size()*kBlockSize,
  // so in_block == 0 must NOT be mistaken for an empty fresh block).
  const bool tail_exists = (next_offset_ >> kBlockShift) < blocks_.size();
  if (!tail_exists || in_block + bytes > kBlockSize) {
    blocks_.emplace_back(kBlockSize, 0);
    next_offset_ = static_cast<uint32_t>((blocks_.size() - 1) * kBlockSize);
  }
  const uint32_t off = next_offset_;
  next_offset_ += bytes;
  return off;
}

uint32_t CompactPostingPool::alloc_slice(int level, uint32_t* slice_end) {
  const uint32_t cap = kSliceSizes[level];
  const uint32_t first = alloc_run(cap + kPtrBytes);
  *slice_end = first + cap;
  // Zero the forward pointer so a not-yet-extended tail slice reads next_head == 0.
  std::memset(at(*slice_end), 0, kPtrBytes);
  return first;
}

uint32_t CompactPostingPool::read_ptr(uint32_t slice_end) const {
  uint32_t v;
  std::memcpy(&v, at(slice_end), sizeof(v));
  return v;
}

void CompactPostingPool::write_ptr(uint32_t slice_end, uint32_t next_head) {
  std::memcpy(at(slice_end), &next_head, sizeof(next_head));
}

uint32_t CompactPostingPool::start_chain(SliceWriter* w, uint8_t* level) {
  *level = 0;
  const uint32_t head = alloc_slice(0, &w->slice_end);
  w->cur = head;
  return head;
}

void CompactPostingPool::append_byte(SliceWriter* w, uint8_t* level, uint8_t value) {
  if (w->cur == w->slice_end) {
    // Current slice payload region is full: grow the chain with a larger slice and
    // record the link in the old slice's trailing pointer bytes.
    const uint8_t next_level = kNextLevel[*level];
    uint32_t new_end = 0;
    const uint32_t new_head = alloc_slice(next_level, &new_end);
    write_ptr(w->slice_end, new_head);
    *level = next_level;
    w->cur = new_head;
    w->slice_end = new_end;
  }
  *at(w->cur) = value;
  ++w->cur;
  ++payload_bytes_;
}

CompactPostingPool::Cursor::Cursor(const CompactPostingPool* pool, uint32_t head,
                                   uint32_t remaining)
    : pool_(pool), cur_(head), level_(0), remaining_(remaining) {
  // The first slice is level 0; its payload region ends kSliceSizes[0] bytes in.
  slice_end_ = head + CompactPostingPool::kSliceSizes[0];
}

uint8_t CompactPostingPool::Cursor::next() {
  if (cur_ == slice_end_) {
    // Reached this slice's boundary: follow the forward pointer to the next slice.
    const uint32_t next_head = pool_->read_ptr(slice_end_);
    level_ = CompactPostingPool::kNextLevel[level_];
    cur_ = next_head;
    slice_end_ = next_head + CompactPostingPool::kSliceSizes[level_];
  }
  const uint8_t v = *pool_->at(cur_);
  ++cur_;
  --remaining_;
  return v;
}

}  // namespace snii::writer
