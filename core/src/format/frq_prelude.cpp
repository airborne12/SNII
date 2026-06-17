#include "snii/format/frq_prelude.h"

#include <cstddef>

#include "snii/encoding/byte_source.h"
#include "snii/encoding/crc32c.h"

namespace snii::format {

namespace {

// Encodes a varint32 column into its own sink.
void encode_varint32_column(const std::vector<uint32_t>& values, ByteSink* out) {
  for (uint32_t v : values) {
    out->put_varint32(v);
  }
}

// Validates that every per-frq-window vector shares length N and that prx data
// is only present when has_prx is set.
Status validate_columns(const FrqPreludeColumns& cols) {
  const size_t n = cols.max_freq.size();
  const bool consistent = cols.max_norm.size() == n &&
                          cols.last_docid_delta.size() == n &&
                          cols.frq_window_len.size() == n &&
                          cols.win_crc32c.size() == n;
  if (!consistent) {
    return Status::InvalidArgument("frq_prelude: per-window column length mismatch");
  }
  if (!cols.has_prx && !cols.prx_cum_off.empty()) {
    return Status::InvalidArgument("frq_prelude: prx_cum_off set while has_prx is false");
  }
  return Status::OK();
}

// Serializes the six (or five) columns, each into its own buffer, in spec order.
// E is appended only when has_prx; the result preserves writing order so col_len[]
// aligns with the on-disk column region.
std::vector<ByteSink> encode_columns(const FrqPreludeColumns& cols) {
  std::vector<ByteSink> columns;
  columns.reserve(6);
  auto push = [&columns](ByteSink&& s) { columns.emplace_back(std::move(s)); };

  ByteSink b;
  encode_varint32_column(cols.max_freq, &b);
  push(std::move(b));

  ByteSink b2;
  for (uint8_t v : cols.max_norm) b2.put_u8(v);
  push(std::move(b2));

  ByteSink c;
  encode_varint32_column(cols.last_docid_delta, &c);
  push(std::move(c));

  ByteSink d;
  encode_varint32_column(cols.frq_window_len, &d);
  push(std::move(d));

  if (cols.has_prx) {
    ByteSink e;
    for (uint64_t v : cols.prx_cum_off) e.put_varint64(v);
    push(std::move(e));
  }

  ByteSink h;
  for (uint32_t v : cols.win_crc32c) h.put_fixed32(v);
  push(std::move(h));
  return columns;
}

uint8_t make_flags(const FrqPreludeColumns& cols) {
  uint8_t flags = 0;
  if (cols.has_freq) flags |= frq_prelude_flags::kHasFreq;
  if (cols.has_prx) flags |= frq_prelude_flags::kHasPrx;
  return flags;
}

// Writes the header (ver, flags, N, [M], col_len[]) into header_sink.
void encode_header(const FrqPreludeColumns& cols, const std::vector<ByteSink>& columns,
                   ByteSink* header_sink) {
  header_sink->put_u8(kFrqPreludeVersion);
  header_sink->put_u8(make_flags(cols));
  header_sink->put_varint64(cols.max_freq.size());
  if (cols.has_prx) {
    header_sink->put_varint64(cols.prx_cum_off.size());
  }
  for (const ByteSink& col : columns) {
    header_sink->put_varint64(col.size());
  }
}

// Decoded header fields shared between the parse and column-decode phases.
struct Header {
  uint8_t ver = 0;
  bool has_freq = false;
  bool has_prx = false;
  uint64_t n = 0;
  uint64_t m = 0;
  std::vector<uint64_t> col_len;  // 5 entries (no prx) or 6 entries (with prx)
};

size_t expected_column_count(bool has_prx) { return has_prx ? 6 : 5; }

Status parse_header(ByteSource* src, Header* h) {
  SNII_RETURN_IF_ERROR(src->get_u8(&h->ver));
  if (h->ver != kFrqPreludeVersion) {
    return Status::Unsupported("frq_prelude: unsupported version");
  }
  uint8_t flags = 0;
  SNII_RETURN_IF_ERROR(src->get_u8(&flags));
  h->has_freq = (flags & frq_prelude_flags::kHasFreq) != 0;
  h->has_prx = (flags & frq_prelude_flags::kHasPrx) != 0;
  SNII_RETURN_IF_ERROR(src->get_varint64(&h->n));
  if (h->has_prx) {
    SNII_RETURN_IF_ERROR(src->get_varint64(&h->m));
  }
  // Anti-DoS: cap window counts from untrusted bytes before any reserve(count).
  // A segment holds at most ~15M docs (>=1 doc/window), so 1<<24 windows is a
  // generous ceiling that still prevents multi-GB allocations on a crafted N/M.
  // (crc32c is not a MAC and cannot defend against a re-stamped inflated count.)
  constexpr uint64_t kMaxWindows = 1ull << 24;
  if (h->n > kMaxWindows || h->m > kMaxWindows) {
    return Status::Corruption("frq_prelude: window count exceeds sane cap");
  }
  const size_t cols = expected_column_count(h->has_prx);
  h->col_len.resize(cols);
  for (size_t i = 0; i < cols; ++i) {
    SNII_RETURN_IF_ERROR(src->get_varint64(&h->col_len[i]));
  }
  return Status::OK();
}

// Verifies the trailing crc32c covering everything before the final fixed32.
Status verify_crc(Slice prelude) {
  if (prelude.size() < sizeof(uint32_t)) {
    return Status::Corruption("frq_prelude: buffer too short for crc");
  }
  const size_t covered = prelude.size() - sizeof(uint32_t);
  uint32_t stored = 0;
  ByteSource crc_src(prelude.subslice(covered, sizeof(uint32_t)));
  SNII_RETURN_IF_ERROR(crc_src.get_fixed32(&stored));
  if (crc32c(prelude.subslice(0, covered)) != stored) {
    return Status::Corruption("frq_prelude: crc32c mismatch");
  }
  return Status::OK();
}

// Decodes a varint32 column expected to hold exactly count entries, and verifies
// it consumed exactly col_len bytes (self-consistency check).
Status decode_varint32_column(Slice col, uint64_t count, std::vector<uint32_t>* out) {
  ByteSource src(col);
  out->clear();
  out->reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    SNII_RETURN_IF_ERROR(src.get_varint32(&v));
    out->push_back(v);
  }
  if (!src.eof()) {
    return Status::Corruption("frq_prelude: varint32 column has trailing bytes");
  }
  return Status::OK();
}

Status decode_u8_column(Slice col, uint64_t count, std::vector<uint8_t>* out) {
  if (col.size() != count) {
    return Status::Corruption("frq_prelude: u8 column length mismatch");
  }
  out->assign(col.data(), col.data() + col.size());
  return Status::OK();
}

Status decode_fixed32_column(Slice col, uint64_t count, std::vector<uint32_t>* out) {
  if (col.size() != count * sizeof(uint32_t)) {
    return Status::Corruption("frq_prelude: fixed32 column length mismatch");
  }
  ByteSource src(col);
  out->clear();
  out->reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    SNII_RETURN_IF_ERROR(src.get_fixed32(&v));
    out->push_back(v);
  }
  return Status::OK();
}

Status decode_varint64_column(Slice col, uint64_t count, std::vector<uint64_t>* out) {
  ByteSource src(col);
  out->clear();
  out->reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t v = 0;
    SNII_RETURN_IF_ERROR(src.get_varint64(&v));
    out->push_back(v);
  }
  if (!src.eof()) {
    return Status::Corruption("frq_prelude: varint64 column has trailing bytes");
  }
  return Status::OK();
}

}  // namespace

Status build_frq_prelude(const FrqPreludeColumns& cols, ByteSink* sink) {
  if (sink == nullptr) {
    return Status::InvalidArgument("frq_prelude: null sink");
  }
  SNII_RETURN_IF_ERROR(validate_columns(cols));

  const std::vector<ByteSink> columns = encode_columns(cols);
  ByteSink body;
  encode_header(cols, columns, &body);
  for (const ByteSink& col : columns) {
    body.put_bytes(col.view());
  }

  sink->put_bytes(body.view());
  sink->put_fixed32(crc32c(body.view()));
  return Status::OK();
}

namespace {

// Splits the column region (immediately after the header, before the crc) into
// per-column slices using col_len[], verifying the total matches exactly.
Status split_columns(Slice prelude, size_t region_start, const std::vector<uint64_t>& col_len,
                     std::vector<Slice>* out) {
  const size_t region_end = prelude.size() - sizeof(uint32_t);  // exclude trailing crc
  size_t cursor = region_start;
  out->clear();
  out->reserve(col_len.size());
  for (uint64_t len : col_len) {
    if (cursor + len > region_end || cursor + len < cursor) {
      return Status::Corruption("frq_prelude: col_len exceeds column region");
    }
    out->push_back(prelude.subslice(cursor, len));
    cursor += len;
  }
  if (cursor != region_end) {
    return Status::Corruption("frq_prelude: col_len sum does not cover column region");
  }
  return Status::OK();
}

// Decodes all columns from their slices into the supplied output vectors, in
// spec order. The E (prx) column is decoded only when has_prx is set.
Status decode_all_columns(const Header& h, const std::vector<Slice>& cols,
                          std::vector<uint32_t>* max_freq, std::vector<uint8_t>* max_norm,
                          std::vector<uint32_t>* last_docid_delta,
                          std::vector<uint32_t>* frq_window_len, std::vector<uint32_t>* win_crc32c,
                          std::vector<uint64_t>* prx_cum_off) {
  size_t idx = 0;
  SNII_RETURN_IF_ERROR(decode_varint32_column(cols[idx++], h.n, max_freq));
  SNII_RETURN_IF_ERROR(decode_u8_column(cols[idx++], h.n, max_norm));
  SNII_RETURN_IF_ERROR(decode_varint32_column(cols[idx++], h.n, last_docid_delta));
  SNII_RETURN_IF_ERROR(decode_varint32_column(cols[idx++], h.n, frq_window_len));
  if (h.has_prx) {
    SNII_RETURN_IF_ERROR(decode_varint64_column(cols[idx++], h.m, prx_cum_off));
  }
  SNII_RETURN_IF_ERROR(decode_fixed32_column(cols[idx++], h.n, win_crc32c));
  return Status::OK();
}

}  // namespace

Status FrqPreludeReader::open(Slice prelude, FrqPreludeReader* out) {
  SNII_RETURN_IF_ERROR(verify_crc(prelude));

  ByteSource src(prelude);
  Header h;
  SNII_RETURN_IF_ERROR(parse_header(&src, &h));

  std::vector<Slice> cols;
  SNII_RETURN_IF_ERROR(split_columns(prelude, src.position(), h.col_len, &cols));

  out->has_freq_ = h.has_freq;
  out->has_prx_ = h.has_prx;
  SNII_RETURN_IF_ERROR(decode_all_columns(h, cols, &out->max_freq_, &out->max_norm_,
                                          &out->last_docid_delta_, &out->frq_window_len_,
                                          &out->win_crc32c_, &out->prx_cum_off_));
  return Status::OK();
}

}  // namespace snii::format
