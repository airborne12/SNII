#pragma once

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"

namespace snii {

// append-only write cursor: all section serialization goes through this; manual byte assembly is forbidden.
// All multi-byte fixed-width fields are little-endian.
class ByteSink {
 public:
  void put_u8(uint8_t v) { buf_.push_back(v); }
  void put_fixed32(uint32_t v);
  void put_fixed64(uint64_t v);
  void put_varint32(uint32_t v);
  void put_varint64(uint64_t v);
  void put_zigzag(int64_t v);
  void put_bytes(Slice s);

  size_t size() const { return buf_.size(); }
  const std::vector<uint8_t>& buffer() const { return buf_; }
  Slice view() const { return Slice(buf_); }

 private:
  std::vector<uint8_t> buf_;
};

}  // namespace snii
