#include "snii/encoding/pfor.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace snii {
namespace {

uint8_t bits_for(uint32_t v) {
  uint8_t b = 0;
  while (v) {
    ++b;
    v >>= 1;
  }
  return b;
}

// 选取使 (packed + 异常) 总字节最小的 bit_width。异常成本按每个约 6 字节估算。
uint8_t choose_width(const uint32_t* v, size_t n) {
  uint8_t maxw = 0;
  for (size_t i = 0; i < n; ++i) maxw = std::max(maxw, bits_for(v[i]));
  uint8_t best = maxw;
  size_t best_cost = SIZE_MAX;
  for (int w = 0; w <= maxw; ++w) {
    size_t exc = 0;
    for (size_t i = 0; i < n; ++i)
      if (bits_for(v[i]) > w) ++exc;
    size_t cost = (static_cast<size_t>(w) * n + 7) / 8 + exc * 6;
    if (cost < best_cost) {
      best_cost = cost;
      best = static_cast<uint8_t>(w);
    }
  }
  return best;
}

uint32_t low_mask(uint8_t w) { return (w >= 32) ? 0xFFFFFFFFu : ((1u << w) - 1u); }

void bitpack(const uint32_t* v, size_t n, uint8_t w, ByteSink* out) {
  if (w == 0) return;
  uint64_t acc = 0;
  int filled = 0;
  for (size_t i = 0; i < n; ++i) {
    acc |= static_cast<uint64_t>(v[i] & low_mask(w)) << filled;
    filled += w;
    while (filled >= 8) {
      out->put_u8(static_cast<uint8_t>(acc));
      acc >>= 8;
      filled -= 8;
    }
  }
  if (filled > 0) out->put_u8(static_cast<uint8_t>(acc));
}

Status bitunpack(ByteSource* src, size_t n, uint8_t w, uint32_t* out) {
  if (w == 0) {
    for (size_t i = 0; i < n; ++i) out[i] = 0;
    return Status::OK();
  }
  uint64_t acc = 0;
  int filled = 0;
  for (size_t i = 0; i < n; ++i) {
    while (filled < w) {
      uint8_t b;
      SNII_RETURN_IF_ERROR(src->get_u8(&b));
      acc |= static_cast<uint64_t>(b) << filled;
      filled += 8;
    }
    out[i] = static_cast<uint32_t>(acc & low_mask(w));
    acc >>= w;
    filled -= w;
  }
  return Status::OK();
}

}  // namespace

void pfor_encode(const uint32_t* values, size_t n, ByteSink* out) {
  uint8_t w = choose_width(values, n);
  std::vector<std::pair<uint32_t, uint32_t>> exc;  // (index, full value)
  std::vector<uint32_t> low(values, values + n);
  for (size_t i = 0; i < n; ++i) {
    if (bits_for(values[i]) > w) {
      exc.emplace_back(static_cast<uint32_t>(i), values[i]);
      low[i] = 0;  // 异常位置低位写 0 占位，真值在异常表
    }
  }
  out->put_u8(w);
  out->put_varint32(static_cast<uint32_t>(exc.size()));
  bitpack(low.data(), n, w, out);
  uint32_t prev = 0;
  for (const auto& e : exc) {
    out->put_varint32(e.first - prev);
    out->put_varint32(e.second);
    prev = e.first;
  }
}

Status pfor_decode(ByteSource* src, size_t n, uint32_t* out) {
  uint8_t w;
  SNII_RETURN_IF_ERROR(src->get_u8(&w));
  uint32_t n_exc;
  SNII_RETURN_IF_ERROR(src->get_varint32(&n_exc));
  SNII_RETURN_IF_ERROR(bitunpack(src, n, w, out));
  uint32_t idx = 0;
  for (uint32_t i = 0; i < n_exc; ++i) {
    uint32_t d, val;
    SNII_RETURN_IF_ERROR(src->get_varint32(&d));
    SNII_RETURN_IF_ERROR(src->get_varint32(&val));
    idx += d;
    if (idx >= n) return Status::Corruption("pfor exception index out of range");
    out[idx] = val;
  }
  return Status::OK();
}

}  // namespace snii
