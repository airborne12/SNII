// BENCH.5 unit tests -- the write-side resource gate (build CPU / RSS / disk).
//
// Pure: zero I/O, zero CLucene, zero corpus. Each test feeds literal resource
// trios into evaluate_resources and asserts the gated verdict against the
// documented 5M bounded-spill numbers and the boundary / blow-up / zero-baseline
// edges from the BENCH.5 spec table.

#include "resource_gate.h"

#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

bench::ResourceTrio T(double cpu_s, double peak_rss_mib, uint64_t index_mib) {
  return bench::ResourceTrio{cpu_s, peak_rss_mib, index_mib * kMiB};
}

// --- documented 5M bounded-spill numbers all pass ---------------------------
// docs §2.8 write-side 5M: RSS SNII 626 vs CLucene 716 (0.87x <= 1.10x); disk
// SNII 159 vs CLucene 148 (1.07x <= 1.10x); CPU SNII 19.7 vs CLucene 18.96
// (1.04x <= 1.50x bounded). Locks the head-line write-side claim.
TEST(ResourceGate, DocumentedFiveMBoundedPasses) {
  const auto v = bench::evaluate_resources(T(18.96, 716, 148), T(19.7, 626, 159),
                                           1.50, 1.10, 1.10);
  EXPECT_TRUE(v.rss_pass);
  EXPECT_TRUE(v.disk_pass);
  EXPECT_TRUE(v.cpu_pass);
  EXPECT_TRUE(v.overall_pass);
  EXPECT_NEAR(v.rss_ratio, 626.0 / 716.0, 1e-9);
  EXPECT_NEAR(v.disk_ratio, 159.0 / 148.0, 1e-9);
  EXPECT_NEAR(v.cpu_ratio, 19.7 / 18.96, 1e-9);
}

// --- the documented numbers also pass via the manifest constants ------------
TEST(ResourceGate, DocumentedFiveMBoundedPassesViaManifestCaps) {
  const auto v = bench::evaluate_resources(
      T(18.96, 716, 148), T(19.7, 626, 159), bench::kResourceCpuCap,
      bench::kResourceRssCap, bench::kResourceDiskCap);
  EXPECT_TRUE(v.overall_pass);
}

// --- disk blow-up turns the row red -----------------------------------------
TEST(ResourceGate, DiskBlowupFails) {
  const auto v = bench::evaluate_resources(T(18.96, 716, 148), T(19.7, 626, 300),
                                           1.50, 1.10, 1.10);
  EXPECT_FALSE(v.disk_pass);
  EXPECT_FALSE(v.overall_pass);
  EXPECT_TRUE(v.rss_pass);  // other axes unaffected
}

// --- RSS cap boundary is inclusive (exact cap PASS, one over FAIL) -----------
TEST(ResourceGate, RssBoundaryInclusive) {
  const auto at = bench::evaluate_resources(T(1, 100, 1), T(1, 110, 1), 1.50,
                                            1.10, 1.10);
  EXPECT_TRUE(at.rss_pass);  // 110/100 == 1.10 exactly -> inclusive PASS
  const auto over = bench::evaluate_resources(T(1, 100, 1), T(1, 111, 1), 1.50,
                                              1.10, 1.10);
  EXPECT_FALSE(over.rss_pass);  // 1.11 > 1.10 -> FAIL
  EXPECT_FALSE(over.overall_pass);
}

// --- CPU cap boundary (1.50x bounded) ---------------------------------------
TEST(ResourceGate, CpuBoundaryInclusive) {
  const auto at = bench::evaluate_resources(T(10, 1, 1), T(15, 1, 1), 1.50, 1.10,
                                            1.10);
  EXPECT_TRUE(at.cpu_pass);  // 15/10 == 1.50 exactly
  const auto over = bench::evaluate_resources(T(10, 1, 1), T(15.5, 1, 1), 1.50,
                                              1.10, 1.10);
  EXPECT_FALSE(over.cpu_pass);  // 1.55 > 1.50
}

// --- zero CLucene baseline guard: non-zero SNII over a 0 baseline FAILS ------
TEST(ResourceGate, ZeroCluceneBaselineGuard) {
  const auto v = bench::evaluate_resources(T(0, 0, 0), T(19.7, 626, 159), 1.50,
                                           1.10, 1.10);
  EXPECT_FALSE(v.cpu_pass);
  EXPECT_FALSE(v.rss_pass);
  EXPECT_FALSE(v.disk_pass);
  EXPECT_FALSE(v.overall_pass);
}

// --- both-zero on an axis is parity (empty index == empty index PASSES) ------
TEST(ResourceGate, IndexBytesZeroBothIsParity) {
  // Empty corpus -> both index_bytes 0; disk_ratio 1.0, disk_pass true. CPU/RSS
  // non-zero and within cap.
  const auto v = bench::evaluate_resources(T(1, 100, 0), T(1, 100, 0), 1.50,
                                           1.10, 1.10);
  EXPECT_TRUE(v.disk_pass);
  EXPECT_NEAR(v.disk_ratio, 1.0, 1e-9);
  EXPECT_TRUE(v.overall_pass);
}

// --- one zero baseline axis fails overall even if the others pass -----------
TEST(ResourceGate, OneZeroBaselineAxisFailsOverall) {
  // CLucene reported 0 disk (degenerate) but non-zero SNII disk -> disk fails,
  // dragging overall down even though CPU/RSS are fine.
  const auto v = bench::evaluate_resources(T(10, 100, 0), T(10, 100, 5), 1.50,
                                           1.10, 1.10);
  EXPECT_TRUE(v.cpu_pass);
  EXPECT_TRUE(v.rss_pass);
  EXPECT_FALSE(v.disk_pass);
  EXPECT_FALSE(v.overall_pass);
}

// --- write-side JSONL emitter: required keys, single trailing newline --------
TEST(ResourceGate, JsonlHasRequiredKeysAndSingleNewline) {
  bench::ResourceRow row;
  row.surface = "local";
  row.doc_count = 150000;
  row.seed = 42;
  row.spill_mib = 16;
  row.git_rev = "abc123";
  row.clucene = T(18.96, 716, 148);
  row.snii = T(19.7, 626, 159);
  row.verdict = bench::evaluate_resources(row.clucene, row.snii, row.cpu_cap,
                                          row.rss_cap, row.disk_cap);
  std::ostringstream os;
  bench::write_resource_jsonl(os, row);
  const std::string s = os.str();

  // Exactly one trailing newline (a JSONL record must be one line).
  ASSERT_FALSE(s.empty());
  EXPECT_EQ(s.back(), '\n');
  EXPECT_EQ(s.find('\n'), s.size() - 1);

  // Required reproduction + verdict keys are present.
  for (const char* key : {"\"metric_kind\":\"resource\"", "\"surface\":\"local\"",
                          "\"doc_count\":150000", "\"seed\":42",
                          "\"spill_mib\":16", "\"git_rev\":\"abc123\"",
                          "\"cpu_s\":", "\"peak_rss_mib\":", "\"index_bytes\":",
                          "\"cpu_ratio\":", "\"rss_ratio\":", "\"disk_ratio\":",
                          "\"cpu_pass\":", "\"rss_pass\":", "\"disk_pass\":",
                          "\"overall_pass\":true"}) {
    EXPECT_NE(s.find(key), std::string::npos) << "missing key: " << key;
  }

  // index_bytes are integer literals, never scientific notation.
  EXPECT_NE(s.find("155189248"), std::string::npos);  // 148 MiB exactly
  EXPECT_EQ(s.find("e+"), std::string::npos);
}

// --- JSONL of a failing row carries overall_pass:false ----------------------
TEST(ResourceGate, JsonlFailingRowMarkedFalse) {
  bench::ResourceRow row;
  row.clucene = T(10, 100, 100);
  row.snii = T(10, 100, 300);  // 3x disk blow-up
  row.verdict = bench::evaluate_resources(row.clucene, row.snii, row.cpu_cap,
                                          row.rss_cap, row.disk_cap);
  std::ostringstream os;
  bench::write_resource_jsonl(os, row);
  const std::string s = os.str();
  EXPECT_NE(s.find("\"disk_pass\":false"), std::string::npos);
  EXPECT_NE(s.find("\"overall_pass\":false"), std::string::npos);
  EXPECT_EQ(s.find("\"overall_pass\":true"), std::string::npos);
}

}  // namespace
