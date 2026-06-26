// Unit tests for the pure golden-metric verdict core (bench::MetricGate).
//
// These tests are I/O-free, CLucene-free and SNII-reader-free: they feed
// hand-written snii::io::IoMetrics literals into evaluate_metric / pick_metric
// / gate_ratio and assert every comparison operator and ratio boundary. This is
// BENCH.1: the single gate through which every later benchmark item's three
// golden metrics (serial_rounds, range_gets, total_request_bytes) are asserted.

#include "metric_gate.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

#include "snii/io/metered_file_reader.h"

namespace {

// Helper to build an IoMetrics with only the three golden metrics that the
// gate cares about: serial_rounds, range_gets, total_request_bytes. read_at and
// remote_bytes are left zero (not gated here).
snii::io::IoMetrics M(uint64_t serial_rounds, uint64_t range_gets,
                      uint64_t total_request_bytes) {
  snii::io::IoMetrics m;
  m.serial_rounds = serial_rounds;
  m.range_gets = range_gets;
  m.total_request_bytes = total_request_bytes;
  return m;
}

// --- gate_ratio: must byte-for-byte replicate bench/main.cpp ratio() ---

TEST(MetricGate, GateRatioReplicatesMainCpp) {
  // sn==0 && cl==0 -> 1.0 ; sn==0 && cl>0 -> (double)cl ; else cl/sn.
  EXPECT_DOUBLE_EQ(bench::gate_ratio(0, 0), 1.0);
  EXPECT_DOUBLE_EQ(bench::gate_ratio(500, 0), 500.0);
  EXPECT_DOUBLE_EQ(bench::gate_ratio(200, 100), 2.0);
}

// --- kLeRatio: SNII <= max_ratio * CLucene (inclusive boundary) ---

TEST(MetricGate, LeRatioBoundaryEqualRatioIsPass) {
  auto v = bench::evaluate_metric(
      M(0, 0, 200), M(0, 0, 100),
      {"total_request_bytes", bench::GateCmp::kLeRatio, 2.0, 0});
  EXPECT_TRUE(v.pass);
  EXPECT_DOUBLE_EQ(v.ratio, 2.0);
  EXPECT_EQ(v.clucene, 200u);
  EXPECT_EQ(v.snii, 100u);
}

TEST(MetricGate, RatioExactBoundaryInclusivePass) {
  // CL/SNII == 1/max_ratio exactly: SNII == max_ratio_cl_over_snii is the goal,
  // expressed as "snii advantage ratio >= 2.0" -> cl=200, snii=100, cap ratio 2.
  auto v = bench::evaluate_metric(
      M(0, 0, 200), M(0, 0, 100),
      {"total_request_bytes", bench::GateCmp::kLeRatio, 2.0, 0});
  EXPECT_TRUE(v.pass);  // inclusive boundary
}

TEST(MetricGate, RatioOneUlpOverFails) {
  // SNII fetched 1 byte too many for a 2.0x advantage: 200/101 < 2.0 -> fail.
  auto v = bench::evaluate_metric(
      M(0, 0, 200), M(0, 0, 101),
      {"total_request_bytes", bench::GateCmp::kLeRatio, 2.0, 0});
  EXPECT_FALSE(v.pass);
}

TEST(MetricGate, MaxRatioZeroOrLessMeansParityOrBetter) {
  // Honest break-even sentinel (max_ratio<=0): pass only if snii <= clucene.
  EXPECT_TRUE(bench::evaluate_metric(
                  M(0, 0, 100), M(0, 0, 100),
                  {"total_request_bytes", bench::GateCmp::kLeRatio, 0.0, 0})
                  .pass);
  EXPECT_TRUE(bench::evaluate_metric(
                  M(0, 0, 100), M(0, 0, 99),
                  {"total_request_bytes", bench::GateCmp::kLeRatio, 0.0, 0})
                  .pass);
  EXPECT_FALSE(bench::evaluate_metric(
                   M(0, 0, 100), M(0, 0, 101),
                   {"total_request_bytes", bench::GateCmp::kLeRatio, 0.0, 0})
                   .pass);
}

// --- kLeAbsolute: snii metric <= max_absolute (inclusive) ---

TEST(MetricGate, SerialRoundsLeAbsoluteBoundary) {
  EXPECT_TRUE(bench::evaluate_metric(
                  M(21, 0, 0), M(5, 0, 0),
                  {"serial_rounds", bench::GateCmp::kLeAbsolute, 0, 5})
                  .pass);
  EXPECT_FALSE(bench::evaluate_metric(
                   M(21, 0, 0), M(6, 0, 0),
                   {"serial_rounds", bench::GateCmp::kLeAbsolute, 0, 5})
                   .pass);
}

// --- kEqual: parity metric must be exactly equal both engines ---

TEST(MetricGate, EqualCmpForDocidIndependentMetric) {
  EXPECT_TRUE(bench::evaluate_metric(
                  M(0, 7, 0), M(0, 7, 0),
                  {"range_gets", bench::GateCmp::kEqual, 0, 0})
                  .pass);
  EXPECT_FALSE(bench::evaluate_metric(
                   M(0, 7, 0), M(0, 8, 0),
                   {"range_gets", bench::GateCmp::kEqual, 0, 0})
                   .pass);
}

// --- div-by-zero guard: snii==0, clucene>0 ---

TEST(MetricGate, SniiZeroCluceneNonzeroDivByZeroGuard) {
  auto v = bench::evaluate_metric(
      M(0, 0, 500), M(0, 0, 0),
      {"total_request_bytes", bench::GateCmp::kLeRatio, 2.0, 0});
  EXPECT_DOUBLE_EQ(v.ratio, 500.0);  // replicates main.cpp ratio()
  EXPECT_TRUE(v.pass);               // 500x >= 2.0x advantage
}

TEST(MetricGate, BothZeroRatioOne) {
  auto v = bench::evaluate_metric(
      M(0, 0, 0), M(0, 0, 0),
      {"total_request_bytes", bench::GateCmp::kLeRatio, 1.0, 0});
  EXPECT_DOUBLE_EQ(v.ratio, 1.0);
  EXPECT_TRUE(v.pass);  // absent term: zero bytes both sides, no false regress
}

// --- pick_metric name resolution + fail-loud on typo ---

TEST(MetricGate, PickMetricResolvesGoldenNames) {
  auto m = M(11, 22, 33);
  EXPECT_EQ(bench::pick_metric(m, "serial_rounds"), 11u);
  EXPECT_EQ(bench::pick_metric(m, "range_gets"), 22u);
  EXPECT_EQ(bench::pick_metric(m, "total_request_bytes"), 33u);
}

TEST(MetricGate, PickMetricUnknownNameThrows) {
  EXPECT_THROW(bench::pick_metric(M(1, 2, 3), "bogus"),
               std::invalid_argument);
}

// --- overflow guard: near-UINT64_MAX must not UB in the double ratio ---

TEST(MetricGate, Uint64MaxNoOverflow) {
  uint64_t cl = uint64_t(1) << 63;  // 2^63
  uint64_t sn = uint64_t(1) << 62;  // 2^62
  auto v = bench::evaluate_metric(
      M(0, 0, cl), M(0, 0, sn),
      {"total_request_bytes", bench::GateCmp::kLeRatio, 2.0, 0});
  EXPECT_NEAR(v.ratio, 2.0, 1e-6);
  EXPECT_TRUE(v.pass);
}

}  // namespace
