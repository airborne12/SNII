// BENCH.6 unit tests -- the real-OSS surface gate (wall-clock + golden rounds).
//
// Pure: zero network, zero CLucene, zero S3. Each test feeds literal wall_ms
// sample vectors plus two (same-cost-model) IoMetrics into median_ms /
// evaluate_oss and asserts the noise-tolerant OSS verdict against the documented
// 5M phrase numbers and the boundary / regression / degenerate edges from the
// BENCH.6 spec table. The OSS verdict is structural where it can be (serial_rounds
// must be parity-or-better, docids must match) and noise-tolerant only on the
// wall-clock axis (median-of-repeats ratio >= a manifest floor).

#include "oss_gate.h"

#include <vector>

#include <gtest/gtest.h>

#include "snii/io/metered_file_reader.h"

namespace {

// An IoMetrics carrying only a serial_rounds count -- the structural axis the OSS
// gate asserts at parity-or-better (the wall-clock axis tolerates noise, the
// round count does not). Mirrors the spec's R(n) shorthand.
snii::io::IoMetrics R(uint64_t serial_rounds) {
  snii::io::IoMetrics m;
  m.serial_rounds = serial_rounds;
  return m;
}

// --- median convention must match main.cpp:median() -------------------------
TEST(OssGate, MedianOfSamples) {
  EXPECT_DOUBLE_EQ(bench::median_ms({100, 300, 200}), 200.0);
  EXPECT_DOUBLE_EQ(bench::median_ms({}), 0.0);
}

// median_odd_even_empty: odd -> middle, even -> upper-middle (v[n/2], matching
// main.cpp:median), empty -> 0. The convention must be byte-identical to the
// printed-distribution median so the gated number equals the reported one.
TEST(OssGate, MedianOddEvenEmpty) {
  EXPECT_DOUBLE_EQ(bench::median_ms({100, 300, 200}), 200.0);  // odd -> 200
  EXPECT_DOUBLE_EQ(bench::median_ms({}), 0.0);                 // empty -> 0
  EXPECT_DOUBLE_EQ(bench::median_ms({5, 15}), 15.0);           // even -> upper (v[1])
}

// documented_5m_phrase_pass: the headline 2.94x S3-native claim. CLucene ~1074ms
// over 21 serial rounds vs SNII ~365ms over 8 rounds, floor 2.0. wall + rounds +
// overall all PASS.
TEST(OssGate, DocumentedFiveMPhrasePasses) {
  const auto v = bench::evaluate_oss({1074, 1100, 1050}, {365, 360, 370}, R(21),
                                     R(8), true, 2.0);
  EXPECT_GE(v.wall_ratio_cl_over_snii, 2.0);
  EXPECT_TRUE(v.wall_pass);
  EXPECT_TRUE(v.rounds_pass);
  EXPECT_TRUE(v.docids_pass);
  EXPECT_TRUE(v.overall_pass);
}

// wall_noise_win_but_rounds_regress: SNII is incidentally faster on the wall
// clock (1000 vs 100) but issues MORE serial rounds (9 vs 5). The wall axis is
// noise; the round count is the structural S3-native claim, so rounds_pass is
// false and overall FAILS regardless of the wall win.
TEST(OssGate, RoundsRegressionFailsEvenIfWallNoiseWins) {
  const auto v = bench::evaluate_oss({1000}, {100}, R(5), R(9), true, 2.0);
  EXPECT_TRUE(v.wall_pass);  // wall noise says SNII faster
  EXPECT_FALSE(v.rounds_pass);
  EXPECT_FALSE(v.overall_pass);
}

// docid_mismatch_over_oss: a real ranged-GET divergence. docids_match=false
// vetoes the verdict no matter how fast SNII is -- correctness over latency.
TEST(OssGate, DocidMismatchFails) {
  const auto v = bench::evaluate_oss({1074}, {300}, R(21), R(8), false, 2.0);
  EXPECT_TRUE(v.wall_pass);
  EXPECT_TRUE(v.rounds_pass);
  EXPECT_FALSE(v.docids_pass);
  EXPECT_FALSE(v.overall_pass);
}

// snii_slower_fails: a real latency regression (median SNII 600 > median CL 500)
// drives wall_ratio < 1 -> wall_pass false -> overall FAIL. A latency regression
// must go red.
TEST(OssGate, SniiSlowerFails) {
  const auto v = bench::evaluate_oss({500}, {600}, R(8), R(8), true, 2.0);
  EXPECT_LT(v.wall_ratio_cl_over_snii, 1.0);
  EXPECT_FALSE(v.wall_pass);
  EXPECT_FALSE(v.overall_pass);
}

// floor_boundary_inclusive: wall_ratio exactly == floor passes (inclusive with
// epsilon). CL 200 vs SNII 100 = 2.0, floor 2.0 -> wall_pass true.
TEST(OssGate, FloorBoundaryInclusive) {
  const auto v = bench::evaluate_oss({200}, {100}, R(8), R(8), true, 2.0);
  EXPECT_DOUBLE_EQ(v.wall_ratio_cl_over_snii, 2.0);
  EXPECT_TRUE(v.wall_pass);
  EXPECT_TRUE(v.overall_pass);
}

// both_walls_zero: a degenerate / mock run with no wall signal on either side.
// No div-by-zero: ratio collapses to 1.0, and wall_pass is true only if the floor
// is <= 1.0 (honest about "no signal"). Here the floor is 2.0, so wall_pass false.
TEST(OssGate, BothWallsZeroNoDivByZero) {
  const auto v = bench::evaluate_oss({}, {}, R(8), R(8), true, 2.0);
  EXPECT_DOUBLE_EQ(v.wall_ratio_cl_over_snii, 1.0);
  EXPECT_FALSE(v.wall_pass);  // floor 2.0 > 1.0 -> no false win on absent signal
}

// both_walls_zero with a parity floor (1.0): ratio 1.0 >= floor 1.0 -> wall_pass
// true. This is the <1M small-scale fallback path (kRealOss floor = parity).
TEST(OssGate, BothWallsZeroParityFloorPasses) {
  const auto v = bench::evaluate_oss({}, {}, R(8), R(8), true, 1.0);
  EXPECT_DOUBLE_EQ(v.wall_ratio_cl_over_snii, 1.0);
  EXPECT_TRUE(v.wall_pass);
  EXPECT_TRUE(v.overall_pass);
}

// rounds parity (equal) is a PASS: serial_rounds equal on both engines is
// parity-or-better. The gate is sn.serial_rounds <= cl.serial_rounds.
TEST(OssGate, RoundsParityIsPass) {
  const auto v = bench::evaluate_oss({200}, {100}, R(8), R(8), true, 2.0);
  EXPECT_TRUE(v.rounds_pass);
}

// min_wall_ratio is carried through into the verdict for JSONL reproduction.
TEST(OssGate, FloorEchoedIntoVerdict) {
  const auto v = bench::evaluate_oss({200}, {100}, R(8), R(8), true, 2.0);
  EXPECT_DOUBLE_EQ(v.min_wall_ratio, 2.0);
}

}  // namespace
