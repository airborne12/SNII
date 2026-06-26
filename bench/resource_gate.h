#pragma once

// BENCH.5 -- write-side resource gate (build CPU / peak RSS / disk size).
//
// The query benchmarks (BENCH.1-4) gate the read-side golden metrics
// (serial_rounds / range_gets / request_bytes) through the SAME MeteredFileReader
// yardstick. This module is the WRITE-side counterpart: given the CLucene control
// and the SNII engine each as a (build_cpu_seconds, peak_rss_mib, index_bytes)
// trio -- measured apples-to-apples under an identical spill/flush budget
// (CLucene RAMBufferSizeMB == SNII spill-mib) -- it produces a structured
// PASS/FAIL verdict against the manifest caps:
//
//     peak RSS  <= 1.10x CLucene   (SNII must not blow up build memory)
//     disk size <= 1.10x CLucene   (the on-disk index must not bloat)
//     build CPU <= 1.50x CLucene   (bounded-spill out-of-core build CPU cap)
//
// Pure: no I/O, no CLucene, no corpus, no build. Unit-testable on the documented
// 5M bounded-spill numbers and on the boundary / blow-up / zero-baseline edges.

#include <cstdint>
#include <ostream>
#include <string>

namespace bench {

// One engine's write-side resource footprint.
struct ResourceTrio {
  double cpu_s = 0.0;          // build CPU seconds (cpu_seconds() delta)
  double peak_rss_mib = 0.0;   // process peak RSS in MiB (peak_rss_mib())
  uint64_t index_bytes = 0;    // on-disk index size in bytes (index_bytes())
};

// The structured outcome of gating all three write-side axes. Each ratio is
// CLucene-relative: snii_value / clucene_value (so > 1.0 means SNII used more).
struct ResourceVerdict {
  double cpu_ratio = 0.0;   // sn.cpu_s / cl.cpu_s
  double rss_ratio = 0.0;   // sn.peak_rss_mib / cl.peak_rss_mib
  double disk_ratio = 0.0;  // sn.index_bytes / cl.index_bytes
  bool cpu_pass = false;    // cpu_ratio <= cpu_cap (+eps)
  bool rss_pass = false;    // rss_ratio <= rss_cap (+eps)
  bool disk_pass = false;   // disk_ratio <= disk_cap (+eps)
  bool overall_pass = false;  // cpu_pass AND rss_pass AND disk_pass
};

// Manifest write-side caps (the single documented source of truth, mirrored from
// docs §2.8 / the BENCH summary table: write-side 5M local).
inline constexpr double kResourceCpuCap = 1.50;   // bounded-spill CPU cap
inline constexpr double kResourceRssCap = 1.10;   // peak RSS cap
inline constexpr double kResourceDiskCap = 1.10;  // disk-size cap

// Gate the SNII trio against the CLucene control under the given caps. Each axis
// passes iff sn/cl <= cap (inclusive, with a 1e-9 epsilon so an exact-cap match
// is a PASS, not an off-by-one FAIL). Zero-baseline guard: if a CLucene axis is
// 0 while the SNII axis is non-zero, that axis FAILS (a non-zero SNII cost can
// never be "<= 0x of nothing" -- no false PASS against a missing baseline). If
// BOTH the CLucene and SNII values of an axis are 0 (e.g. an empty index on both
// sides), the ratio is 1.0 and the axis PASSES (empty == empty is parity).
ResourceVerdict evaluate_resources(const ResourceTrio& clucene,
                                   const ResourceTrio& snii, double cpu_cap,
                                   double rss_cap, double disk_cap);

// One write-side benchmark row: the two engines' resource trios, the verdict,
// and the reproduction metadata (corpus scale, seed, spill budget, git rev,
// caps) needed to replay it. surface="local" cost-model write-side bench.
struct ResourceRow {
  std::string surface = "local";  // write-side resource bench is local
  uint32_t doc_count = 0;         // corpus scale (drives 150K/5M bucket)
  uint64_t seed = 0;              // corpus_gen seed (reproducibility)
  uint32_t spill_mib = 0;         // identical spill/flush budget for both engines
  std::string git_rev;            // git revision the row was produced at
  ResourceTrio clucene;
  ResourceTrio snii;
  double cpu_cap = kResourceCpuCap;
  double rss_cap = kResourceRssCap;
  double disk_cap = kResourceDiskCap;
  ResourceVerdict verdict;
};

// Write exactly one JSON object followed by exactly one trailing '\n' describing
// a write-side resource row: the cpu/rss/disk values + ratios + pass flags +
// caps + reproduction metadata. index_bytes are integer literals (PRIu64); the
// cpu/rss/ratio doubles are written with fixed precision (never sci-notation).
// metric_kind is tagged "resource" so a reader can distinguish write-side rows
// from the read-side golden-metric rows of BENCH.3.
void write_resource_jsonl(std::ostream& os, const ResourceRow& row);

}  // namespace bench
