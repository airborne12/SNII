#include "snii/format/xfilter.h"

#include <algorithm>
#include <cmath>

#include "snii/encoding/byte_source.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/format_constants.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace snii::format {

namespace {

// ---- Binary fuse 8 parameters ----------------------------------------------------------------------------
// Standard reference parameters for arity-3, 8-bit binary fuse filters. The capacity factor is ~1.125 and
// the segment length is derived from log2(size). These match the widely used "binaryfuse8" layout so the
// peeling step succeeds with high probability and the false-positive rate is ~0.4%.

struct FuseParams {
  uint32_t segment_length = 0;
  uint32_t segment_count_length = 0;  // segment_count * segment_length (range of the h0 multiply-high map)
  uint32_t array_length = 0;
};

constexpr uint32_t kArity = 3;

uint32_t segment_length_for(uint32_t size) {
  if (size == 0) return 4;
  const double logf = std::log(static_cast<double>(size));
  return static_cast<uint32_t>(1u) << static_cast<uint32_t>(std::floor(logf / std::log(3.33) + 2.25));
}

double size_factor_for(uint32_t size) {
  if (size <= 1) return 0.0;
  const double logf = std::log(static_cast<double>(size));
  return std::max(1.125, 0.875 + 0.25 * std::log(1000000.0) / logf);
}

// Compute the binary-fuse-8 layout parameters for n distinct keys.
FuseParams compute_params(uint32_t size) {
  FuseParams p;
  uint32_t segment_length = segment_length_for(size);
  if (segment_length > 262144) segment_length = 262144;

  const double size_factor = size_factor_for(size);
  const uint32_t capacity =
      size > 1 ? static_cast<uint32_t>(std::round(static_cast<double>(size) * size_factor)) : 0;

  const uint32_t cap_segments = (capacity + segment_length - 1) / segment_length;
  uint32_t init_segment_count = cap_segments > (kArity - 1) ? cap_segments - (kArity - 1) : 1;
  uint32_t array_length = (init_segment_count + kArity - 1) * segment_length;
  uint32_t segment_count = (array_length + segment_length - 1) / segment_length;
  if (segment_count <= kArity - 1) {
    segment_count = 1;
  } else {
    segment_count = segment_count - (kArity - 1);
  }
  array_length = (segment_count + kArity - 1) * segment_length;

  p.segment_length = segment_length;
  p.array_length = array_length;
  // Canonical binary-fuse-8: h0 = mulhi(hash, segment_count * segment_length); h1/h2 are the next two
  // segments. array_length = (segment_count + 2) * segment_length keeps every position in range.
  p.segment_count_length = segment_count * segment_length;
  return p;
}

// ---- Hashing ---------------------------------------------------------------------------------------------

uint64_t mix_seed(uint64_t key, uint64_t seed) {
  uint64_t h = key + seed;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

// Top 16 bits used as the 8-bit fingerprint source (only low 8 bits are kept by the caller).
uint8_t fingerprint_of(uint64_t hash) {
  return static_cast<uint8_t>(hash ^ (hash >> 8) ^ (hash >> 16) ^ (hash >> 24));
}

// 64-bit multiply-high: high 64 bits of the 128-bit product a * b. Maps hash uniformly into [0, n).
uint64_t mulhi(uint64_t a, uint64_t b) {
  return static_cast<uint64_t>((static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b)) >> 64);
}

// Derive the 3 slot positions for a hashed key (canonical binary-fuse-8 scheme).
// h0 = mulhi(hash, segment_count_length) lands within the first segment_count segments; h1/h2 are the two
// following segments. Only h1/h2 are XORed with a low offset masked to (segment_length - 1) bits, so each
// position stays inside its segment. segment_length is always a power of two, so the mask is exact and no
// position can reach array_length = (segment_count + 2) * segment_length.
void slots_of(uint64_t hash, const FuseParams& p, uint32_t out[kArity]) {
  const uint32_t mask = p.segment_length - 1;
  out[0] = static_cast<uint32_t>(mulhi(hash, p.segment_count_length));
  out[1] = out[0] + p.segment_length;
  out[2] = out[1] + p.segment_length;
  out[1] ^= static_cast<uint32_t>(hash >> 18) & mask;
  out[2] ^= static_cast<uint32_t>(hash) & mask;
}

// ---- Peeling state ---------------------------------------------------------------------------------------

struct PeelState {
  std::vector<uint8_t> count;    // number of keys currently touching each slot
  std::vector<uint64_t> xor_h;   // XOR of hashes touching each slot
  std::vector<uint32_t> queue;   // slot indices with count == 1
  std::vector<uint32_t> order;   // peel order: slots
  std::vector<uint64_t> order_h; // peel order: hashes
};

void touch_slots(PeelState* st, uint64_t hash, const uint32_t s[kArity]) {
  for (uint32_t i = 0; i < kArity; ++i) {
    st->count[s[i]]++;
    st->xor_h[s[i]] ^= hash;
  }
}

void untouch_slots(PeelState* st, uint64_t hash, const uint32_t s[kArity]) {
  for (uint32_t i = 0; i < kArity; ++i) {
    st->count[s[i]]--;
    st->xor_h[s[i]] ^= hash;
  }
}

void seed_queue(PeelState* st) {
  for (uint32_t i = 0; i < st->count.size(); ++i) {
    if (st->count[i] == 1) st->queue.push_back(i);
  }
}

// Peel all degree-1 slots. Returns true if every key was peeled (success).
bool peel(PeelState* st, const FuseParams& p) {
  while (!st->queue.empty()) {
    const uint32_t slot = st->queue.back();
    st->queue.pop_back();
    if (st->count[slot] != 1) continue;
    const uint64_t hash = st->xor_h[slot];
    uint32_t s[kArity];
    slots_of(hash, p, s);
    untouch_slots(st, hash, s);
    st->order.push_back(slot);
    st->order_h.push_back(hash);
    for (uint32_t i = 0; i < kArity; ++i) {
      if (st->count[s[i]] == 1) st->queue.push_back(s[i]);
    }
  }
  return st->order.size() == st->order_h.size() && !st->order.empty();
}

// Assign fingerprints in reverse peel order so each key owns exactly one freshly-assigned slot.
void assign_fingerprints(const PeelState& st, const FuseParams& p, std::vector<uint8_t>* fp) {
  for (size_t k = st.order.size(); k-- > 0;) {
    const uint64_t hash = st.order_h[k];
    const uint32_t slot = st.order[k];
    uint32_t s[kArity];
    slots_of(hash, p, s);
    uint8_t value = fingerprint_of(hash);
    for (uint32_t i = 0; i < kArity; ++i) {
      if (s[i] != slot) value ^= (*fp)[s[i]];
    }
    (*fp)[slot] = value;
  }
}

// One construction attempt for a fixed seed. Returns true and fills *fp on success.
bool try_build(const std::vector<uint64_t>& keys, uint64_t seed, const FuseParams& p,
               std::vector<uint8_t>* fp) {
  PeelState st;
  st.count.assign(p.array_length, 0);
  st.xor_h.assign(p.array_length, 0);
  st.order.reserve(keys.size());
  st.order_h.reserve(keys.size());

  for (uint64_t key : keys) {
    const uint64_t hash = mix_seed(key, seed);
    uint32_t s[kArity];
    slots_of(hash, p, s);
    touch_slots(&st, hash, s);
  }
  seed_queue(&st);
  if (!peel(&st, p)) return false;
  if (st.order.size() != keys.size()) return false;

  fp->assign(p.array_length, 0);
  assign_fingerprints(st, p, fp);
  return true;
}

// ---- Key extraction --------------------------------------------------------------------------------------

std::vector<uint64_t> distinct_keys(const std::vector<std::string>& terms) {
  std::vector<uint64_t> keys;
  keys.reserve(terms.size());
  for (const auto& t : terms) keys.push_back(hash_term(t));
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

// Sorts + dedups a key vector in place (the only normalization build_xfilter and
// build_xfilter_hashed need so both produce byte-identical output for the same
// term set).
void dedup_keys(std::vector<uint64_t>* keys) {
  std::sort(keys->begin(), keys->end());
  keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
}

// ---- Serialization ---------------------------------------------------------------------------------------

void serialize(uint64_t seed, const FuseParams& p, const std::vector<uint8_t>& fp, ByteSink* sink) {
  ByteSink payload;
  payload.put_fixed64(seed);
  payload.put_fixed32(p.segment_length);
  payload.put_fixed32(p.segment_count_length);
  payload.put_fixed32(p.array_length);
  payload.put_bytes(Slice(fp));
  SectionFramer::write(*sink, static_cast<uint8_t>(SectionType::kXFilter), payload.view());
}

// Builds + serializes the filter from already-deduped, ascending keys. Shared by
// build_xfilter (terms -> distinct_keys) and build_xfilter_hashed (pre-hashed
// keys), so both emit byte-identical bytes for the same term set.
Status build_from_keys(const std::vector<uint64_t>& keys, ByteSink* sink) {
  if (keys.empty()) {
    serialize(0, FuseParams{}, std::vector<uint8_t>{}, sink);
    return Status::OK();
  }

  const FuseParams p = compute_params(static_cast<uint32_t>(keys.size()));
  std::vector<uint8_t> fp;
  // The reference construction succeeds in the first few attempts with very high probability.
  constexpr int kMaxAttempts = 100;
  uint64_t seed = 0x726ec96f4e36uLL;  // arbitrary nonzero starting seed
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (try_build(keys, seed, p, &fp)) {
      serialize(seed, p, fp, sink);
      return Status::OK();
    }
    seed = mix_seed(seed, 0x9E3779B97F4A7C15uLL) | 1uLL;  // derive a fresh nonzero seed
  }
  return Status::Internal("xfilter: binary-fuse-8 peeling failed after retry budget");
}

}  // namespace

uint64_t hash_term(std::string_view term) {
  return XXH3_64bits(term.data(), term.size());
}

Status build_xfilter(const std::vector<std::string>& terms, ByteSink* sink) {
  if (sink == nullptr) {
    return Status::InvalidArgument("xfilter: sink is null");
  }
  return build_from_keys(distinct_keys(terms), sink);
}

Status build_xfilter_hashed(std::vector<uint64_t> keys, ByteSink* sink) {
  if (sink == nullptr) {
    return Status::InvalidArgument("xfilter: sink is null");
  }
  dedup_keys(&keys);
  return build_from_keys(keys, sink);
}

namespace {

// Re-derive slot positions at query time using the same scheme as construction.
void query_slots(uint64_t hash, uint32_t segment_length, uint32_t segment_count_length,
                 uint32_t out[kArity]) {
  FuseParams p;
  p.segment_length = segment_length;
  p.segment_count_length = segment_count_length;
  slots_of(hash, p, out);
}

}  // namespace

Status XFilterReader::open(Slice section, XFilterReader* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("xfilter: out is null");
  }
  ByteSource src(section);
  FramedSection sec;
  SNII_RETURN_IF_ERROR(SectionFramer::read(src, &sec));
  if (sec.type != static_cast<uint8_t>(SectionType::kXFilter)) {
    return Status::InvalidArgument("xfilter: not a kXFilter section");
  }

  ByteSource ps(sec.payload);
  uint64_t seed = 0;
  uint32_t segment_length = 0;
  uint32_t segment_count_length = 0;
  uint32_t array_length = 0;
  SNII_RETURN_IF_ERROR(ps.get_fixed64(&seed));
  SNII_RETURN_IF_ERROR(ps.get_fixed32(&segment_length));
  SNII_RETURN_IF_ERROR(ps.get_fixed32(&segment_count_length));
  SNII_RETURN_IF_ERROR(ps.get_fixed32(&array_length));
  if (ps.remaining() != array_length) {
    return Status::Corruption("xfilter: fingerprint length mismatch");
  }
  Slice fp;
  SNII_RETURN_IF_ERROR(ps.get_bytes(array_length, &fp));

  *out = XFilterReader{};
  out->seed_ = seed;
  out->segment_length_ = segment_length;
  out->segment_count_length_ = segment_count_length;
  out->fingerprints_.assign(fp.data(), fp.data() + fp.size());
  return Status::OK();
}

bool XFilterReader::maybe_contains(std::string_view term) const {
  if (fingerprints_.empty()) return false;
  const uint64_t hash = mix_seed(hash_term(term), seed_);
  uint32_t s[kArity];
  query_slots(hash, segment_length_, segment_count_length_, s);
  const uint8_t expected = fingerprint_of(hash);
  uint8_t acc = 0;
  for (uint32_t i = 0; i < kArity; ++i) {
    if (s[i] >= fingerprints_.size()) return false;
    acc ^= fingerprints_[s[i]];
  }
  return acc == expected;
}

}  // namespace snii::format
