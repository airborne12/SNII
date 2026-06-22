// XFilter (binary-fuse-8) vs Block-Split Bloom Filter (BSBF) query-cost benchmark.
//
// Question: SNII's XF is loaded WHOLE at index-open to probe a term; at large vocab
// that read dwarfs the actual query I/O. A block-split bloom filter (Parquet/Doris
// BSBF) can probe ONE 32-byte block on demand. This bench compares both on filter
// size, FPR, and the per-query I/O (filter read + downstream dict-block read for the
// "maybe present" path), separately for ABSENT and PRESENT terms, under a cloud
// (1 MiB FileCache block) and a local (4 KiB) read-granularity model.
//
// BSBF here is a faithful reimplementation of the Parquet/Doris block-split bloom
// (256-bit block, 8 SALT masks, bucket = (hash>>32) & (nblocks-1), Parquet's
// OptimalNumOfBytes sizing), keyed by the SAME XXH3 hash SNII's XF uses, so the
// comparison isolates the FILTER STRUCTURE, not the hash.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "snii/encoding/byte_sink.h"
#include "snii/format/xfilter.h"
#include "snii/io/local_file.h"
#include "snii/io/metered_file_reader.h"

namespace {

using snii::ByteSink;
using snii::Slice;
using snii::format::XFilterReader;
using snii::format::build_xfilter_hashed;
using snii::format::hash_term;

// --- Block-split bloom filter (Parquet/Doris BSBF algorithm) ---
// Canonical Parquet/Doris split-block bloom SALT (8 odd 32-bit constants).
constexpr uint32_t kSalt[8] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
                               0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};
constexpr uint32_t kBytesPerBlock = 32;            // 256-bit block = 8 x uint32
constexpr uint32_t kMaxBloomBytes = 128u * 1024 * 1024;
constexpr uint32_t kMinBloomBytes = 32;

// Parquet BlockSplitBloomFilter::OptimalNumOfBytes (header-inline formula).
uint32_t optimal_num_bytes(uint32_t ndv, double fpp) {
  const double m = -8.0 * ndv / std::log(1 - std::pow(fpp, 1.0 / 8));
  uint32_t num_bits;
  if (m < 0 || m > static_cast<double>(kMaxBloomBytes) * 8) {
    num_bits = kMaxBloomBytes << 3;
  } else {
    num_bits = static_cast<uint32_t>(m);
  }
  if (num_bits < (kMinBloomBytes << 3)) num_bits = kMinBloomBytes << 3;
  if (num_bits & (num_bits - 1)) {  // next power of 2
    uint32_t p = 1;
    while (p < num_bits) p <<= 1;
    num_bits = p;
  }
  if (num_bits > (kMaxBloomBytes << 3)) num_bits = kMaxBloomBytes << 3;
  return num_bits >> 3;
}

inline void block_masks(uint32_t key, uint32_t m[8]) {
  for (int i = 0; i < 8; ++i) m[i] = 1u << ((key * kSalt[i]) >> 27);
}

struct Bsbf {
  std::vector<uint32_t> words;  // num_bytes/4
  uint32_t nblocks = 0;

  void init(uint32_t ndv, double fpp) {
    const uint32_t nb = optimal_num_bytes(ndv, fpp);
    nblocks = nb / kBytesPerBlock;
    words.assign(nb / 4, 0u);
  }
  void insert(uint64_t hash) {
    const uint32_t b = static_cast<uint32_t>(hash >> 32) & (nblocks - 1);
    uint32_t m[8];
    block_masks(static_cast<uint32_t>(hash), m);
    for (int i = 0; i < 8; ++i) words[b * 8 + i] |= m[i];
  }
  bool find_mem(uint64_t hash) const {
    const uint32_t b = static_cast<uint32_t>(hash >> 32) & (nblocks - 1);
    uint32_t m[8];
    block_masks(static_cast<uint32_t>(hash), m);
    for (int i = 0; i < 8; ++i)
      if ((words[b * 8 + i] & m[i]) != m[i]) return false;
    return true;
  }
  const uint8_t* bytes() const {
    return reinterpret_cast<const uint8_t*>(words.data());
  }
  size_t size_bytes() const { return words.size() * 4; }

  // On-demand probe: read exactly ONE 32-byte block via the (metered) reader.
  static bool find_ondemand(snii::io::FileReader* r, uint64_t base, uint32_t nblocks,
                            uint64_t hash) {
    const uint32_t b = static_cast<uint32_t>(hash >> 32) & (nblocks - 1);
    std::vector<uint8_t> blk;
    if (!r->read_at(base + static_cast<uint64_t>(b) * kBytesPerBlock, kBytesPerBlock,
                    &blk)
             .ok())
      return true;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(blk.data());
    uint32_t m[8];
    block_masks(static_cast<uint32_t>(hash), m);
    for (int i = 0; i < 8; ++i)
      if ((w[i] & m[i]) != m[i]) return false;
    return true;
  }
};

double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void write_file(const std::string& path, const uint8_t* data, size_t len) {
  snii::io::LocalFileWriter w;
  if (!w.open(path).ok()) { std::fprintf(stderr, "open %s\n", path.c_str()); std::exit(1); }
  w.append(Slice(data, len));
  w.finalize();
}

uint64_t mib_blocks(uint64_t bytes, uint64_t block) {
  return bytes == 0 ? 0 : ((bytes + block - 1) / block) * block;
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t ndv = 2000000;       // vocab size
  double fpp = 0.01;            // BSBF target false-positive prob
  uint32_t queries = 20000;    // absent + present query counts
  uint32_t dict_bytes = 65536;  // downstream dict-block read on "maybe present"
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string f = argv[i];
    if (f == "--ndv") ndv = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    else if (f == "--fpp") fpp = std::strtod(argv[i + 1], nullptr);
    else if (f == "--queries") queries = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    else if (f == "--dict-bytes") dict_bytes = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
  }

  // 1. Vocab + keys (same XXH3 key for both filters).
  std::vector<uint64_t> keys(ndv);
  for (uint32_t i = 0; i < ndv; ++i) keys[i] = hash_term("t" + std::to_string(i));

  // 2. Build XF (fuse-8) and BSBF (block-split) over the same keys.
  ByteSink xf_sink;
  if (!build_xfilter_hashed(keys, &xf_sink).ok()) { std::fprintf(stderr, "xf build\n"); return 1; }
  const std::vector<uint8_t>& xf_blob = xf_sink.buffer();
  Bsbf bsbf;
  bsbf.init(ndv, fpp);
  for (uint64_t k : keys) bsbf.insert(k);

  const size_t xf_size = xf_blob.size();
  const size_t bsbf_size = bsbf.size_bytes();

  // 3. FPR over `queries` absent terms (in-memory).
  XFilterReader xf_reader;
  XFilterReader::open(Slice(xf_blob), &xf_reader);
  uint32_t xf_fp = 0, bsbf_fp = 0;
  for (uint32_t i = 0; i < queries; ++i) {
    const std::string term = "absent_z_" + std::to_string(i);
    if (xf_reader.maybe_contains(term)) ++xf_fp;
    if (bsbf.find_mem(hash_term(term))) ++bsbf_fp;
  }
  const double xf_fpr = static_cast<double>(xf_fp) / queries;
  const double bsbf_fpr = static_cast<double>(bsbf_fp) / queries;

  // 4. Serialize both to disk for metered reads.
  const std::string xf_path = "/tmp/snii_xf.bin", bsbf_path = "/tmp/snii_bsbf.bin";
  write_file(xf_path, xf_blob.data(), xf_size);
  write_file(bsbf_path, bsbf.bytes(), bsbf_size);

  // 5. Measure the cold per-query FILTER read + wall via metered readers, for absent
  //    and present query sets, under 1 MiB (cloud) and 4 KiB (local) block models.
  std::printf("=== XF (fuse-8) vs BSBF (block-split bloom) -- ndv=%u fpp=%.3f Q=%u dict=%uB ===\n",
              ndv, fpp, queries, dict_bytes);
  std::printf("filter size:  XF=%zu B (%.2f bit/key)   BSBF=%zu B (%.2f bit/key)\n",
              xf_size, xf_size * 8.0 / ndv, bsbf_size, bsbf_size * 8.0 / ndv);
  std::printf("measured FPR: XF=%.4f   BSBF=%.4f\n", xf_fpr, bsbf_fpr);

  snii::io::LocalFileReader xf_local, bsbf_local;
  xf_local.open(xf_path);
  bsbf_local.open(bsbf_path);

  for (uint64_t block : {uint64_t(1024 * 1024), uint64_t(4096)}) {
    snii::io::MeteredFileReader xf_m(&xf_local, block), bsbf_m(&bsbf_local, block);
    const char* label = block == 1024 * 1024 ? "cloud(1MiB block)" : "local(4KiB block)";

    // Average a real cold probe (reset metrics each probe). For XF the cold probe
    // loads the WHOLE filter; for BSBF it reads ONE 32B block. Use present keys for
    // the timed reads (filter read is term-independent).
    const uint32_t sample = queries < 2000 ? queries : 2000;
    double xf_wall = 0, bsbf_wall = 0;
    uint64_t xf_req = 0, xf_rem = 0, xf_rounds = 0;
    uint64_t bsbf_req = 0, bsbf_rem = 0, bsbf_rounds = 0;
    for (uint32_t i = 0; i < sample; ++i) {
      const uint64_t key = keys[i % ndv];
      // XF cold: read whole filter, open, probe in-memory.
      xf_m.reset_metrics();
      double t0 = now_ms();
      std::vector<uint8_t> whole;
      xf_m.read_at(0, xf_size, &whole);
      XFilterReader r;
      XFilterReader::open(Slice(whole), &r);
      volatile bool a = r.maybe_contains("t" + std::to_string(i % ndv));
      (void)a;
      xf_wall += now_ms() - t0;
      auto xm = xf_m.metrics();
      xf_req += xm.total_request_bytes; xf_rem += xm.remote_bytes; xf_rounds += xm.serial_rounds;
      // BSBF cold: one on-demand block read.
      bsbf_m.reset_metrics();
      t0 = now_ms();
      volatile bool b = Bsbf::find_ondemand(&bsbf_m, 0, bsbf.nblocks, key);
      (void)b;
      bsbf_wall += now_ms() - t0;
      auto bm = bsbf_m.metrics();
      bsbf_req += bm.total_request_bytes; bsbf_rem += bm.remote_bytes; bsbf_rounds += bm.serial_rounds;
    }
    std::printf("\n[%s] cold per-probe FILTER read (avg over %u):\n", label, sample);
    std::printf("  XF  (load-whole): req=%llu B  remote(block-aligned)=%llu B  rounds=%.2f  wall=%.4f ms\n",
                (unsigned long long)(xf_req / sample), (unsigned long long)(xf_rem / sample),
                (double)xf_rounds / sample, xf_wall / sample);
    std::printf("  BSBF(on-demand 1 block): req=%llu B  remote(block-aligned)=%llu B  rounds=%.2f  wall=%.6f ms\n",
                (unsigned long long)(bsbf_req / sample), (unsigned long long)(bsbf_rem / sample),
                (double)bsbf_rounds / sample, bsbf_wall / sample);

    // Complete query (filter + dict-block read on "maybe present"), block-aligned
    // remote bytes. ABSENT (true negative -> no dict) vs PRESENT (-> +dict).
    const uint64_t dictB = mib_blocks(dict_bytes, block);
    const uint64_t xf_remB = mib_blocks(xf_size, block);
    const uint64_t bsbf_remB = block;  // one 32B block rounds to one cache block
    std::printf("  complete query remote bytes (filter + dict-if-maybe), block-aligned:\n");
    std::printf("    no-filter:  absent=%llu  present=%llu\n",
                (unsigned long long)dictB, (unsigned long long)dictB);
    std::printf("    XF:         absent=%llu  present=%llu\n",
                (unsigned long long)xf_remB, (unsigned long long)(xf_remB + dictB));
    std::printf("    BSBF:       absent=%llu  present=%llu\n",
                (unsigned long long)bsbf_remB, (unsigned long long)(bsbf_remB + dictB));
  }

  std::remove(xf_path.c_str());
  std::remove(bsbf_path.c_str());
  return 0;
}
