// BENCH.4 unit tests -- the scenario-suite gate (build_row + all_passed).
//
// Pure: zero I/O, zero CLucene, zero corpus. Each test feeds literal IoMetrics
// (the SAME yardstick both engines are measured through) into build_row and
// asserts the gated verdict, then asserts all_passed aggregates correctly.

#include "scenario_gate.h"

#include <vector>

#include <gtest/gtest.h>

#include "snii/io/metered_file_reader.h"

namespace {

// IoMetrics literal: (serial_rounds, range_gets, total_request_bytes). Matches
// the convention used by metric_gate_test.cpp / bench_jsonl_test.cpp.
snii::io::IoMetrics M(uint64_t serial_rounds, uint64_t range_gets,
                      uint64_t total_request_bytes) {
  snii::io::IoMetrics m;
  m.serial_rounds = serial_rounds;
  m.range_gets = range_gets;
  m.total_request_bytes = total_request_bytes;
  return m;
}

// --- healthy row passes -----------------------------------------------------
// PHRASE-5 @ 5M manifest: serial_rounds absolute cap 8, request_bytes ratio cap
// 5.0x. CLucene 16 rounds / 10.77MB vs SNII 8 rounds / 0.96MB => 8<=8 and
// 10770000/960000 = 11.2x >= 5.0x. docids match. -> overall_pass.
TEST(ScenarioGate, HealthyPhraseRowPasses) {
  const auto r = bench::build_row("PHRASE-5", "local", 5'000'000u, 42, "abc",
                                  10, true, M(16, 10, 10770000), M(8, 10, 960000));
  EXPECT_TRUE(r.overall_pass);
  EXPECT_EQ(r.verdicts.size(), 2u);  // serial_rounds cap + request_bytes ratio
}

// --- byte-cap breach fails (the core deliverable) ---------------------------
// SNII bytes (0.95MB) are nearly equal to CLucene (1.0MB): ratio ~1.05x, far
// below the 5.0x request_bytes cap. serial_rounds still in cap. docids match.
// The real byte regression must turn the row red.
TEST(ScenarioGate, ByteCapBreachFails) {
  const auto r = bench::build_row("PHRASE-5", "local", 5'000'000u, 42, "abc",
                                  10, true, M(16, 10, 1000000), M(8, 10, 950000));
  EXPECT_FALSE(r.overall_pass);
}

// --- docid mismatch dominates even with perfect metrics ---------------------
TEST(ScenarioGate, DocidMismatchForcesFailEvenIfMetricsGreat) {
  const auto r = bench::build_row("PHRASE-5", "local", 5'000'000u, 42, "abc",
                                  10, false, M(16, 10, 10770000), M(8, 10, 1));
  EXPECT_FALSE(r.overall_pass);
}

// --- absent (df=0) term passes on docid equality alone, no metric gates -----
TEST(ScenarioGate, AbsentTermPassesOnDocidEqualityAlone) {
  const auto r = bench::build_row("TERM-absent", "local", 5'000'000u, 42, "abc",
                                  0, true, M(0, 0, 0), M(0, 0, 0));
  EXPECT_TRUE(r.overall_pass);
  EXPECT_TRUE(r.verdicts.empty());  // df=0 -> docid-equality only
}

// --- absent term with mismatched docids still fails -------------------------
TEST(ScenarioGate, AbsentTermDocidMismatchFails) {
  const auto r = bench::build_row("TERM-absent", "local", 5'000'000u, 42, "abc",
                                  0, false, M(0, 0, 0), M(0, 0, 0));
  EXPECT_FALSE(r.overall_pass);
  EXPECT_TRUE(r.verdicts.empty());
}

// --- match-all: parity-or-better serial_rounds, no spurious byte gate -------
TEST(ScenarioGate, MatchAllGatesParityNotBytes) {
  // Both engines scan the full corpus: equal rounds, no byte-advantage claim.
  const auto r = bench::build_row("MATCH-ALL", "local", 5'000'000u, 42, "abc",
                                  5'000'000, true, M(2, 1, 4000000),
                                  M(2, 1, 4000000));
  EXPECT_TRUE(r.overall_pass);
  ASSERT_EQ(r.verdicts.size(), 1u);  // serial_rounds parity only
  EXPECT_EQ(r.verdicts[0].metric_name, "serial_rounds");
}

// --- 150K vs 5M threshold switch: a row passing at 150K may fail at 5M ------
// PHRASE-5 @ 150K (small): byte gate is parity-or-better (snii <= clucene).
// PHRASE-5 @ 5M (large): byte gate is a 5.0x advantage. Same near-parity bytes
// pass the loose small cap but fail the tight large cap.
TEST(ScenarioGate, ScaleSwitchTightensCap) {
  const auto small = bench::build_row("PHRASE-5", "local", 150'000u, 42, "abc",
                                      10, true, M(3, 1, 1000000),
                                      M(3, 1, 950000));
  EXPECT_TRUE(small.overall_pass);  // parity (950000 <= 1000000)
  const auto big = bench::build_row("PHRASE-5", "local", 5'000'000u, 42, "abc",
                                    10, true, M(8, 1, 1000000), M(8, 1, 950000));
  EXPECT_FALSE(big.overall_pass);  // 1.05x < 5.0x cap
}

// --- serial_rounds absolute cap is enforced ---------------------------------
TEST(ScenarioGate, SerialRoundsAbsoluteCapBreachFails) {
  // PHRASE-5 @ 5M caps serial_rounds at 8; SNII at 9 must fail even with a
  // huge byte advantage.
  const auto r = bench::build_row("PHRASE-5", "local", 5'000'000u, 42, "abc",
                                  10, true, M(16, 10, 10770000), M(9, 10, 960000));
  EXPECT_FALSE(r.overall_pass);
}

// --- all_passed aggregation -------------------------------------------------
TEST(ScenarioGate, AllPassedTrueWhenEveryRowPasses) {
  std::vector<bench::BenchRow> rows;
  rows.push_back(bench::build_row("PHRASE-5", "local", 5'000'000u, 42, "abc", 10,
                                  true, M(16, 10, 10770000), M(8, 10, 960000)));
  rows.push_back(bench::build_row("TERM-absent", "local", 5'000'000u, 42, "abc",
                                  0, true, M(0, 0, 0), M(0, 0, 0)));
  EXPECT_TRUE(bench::all_passed(rows));
}

TEST(ScenarioGate, AllPassedFalseWhenAnyRowFails) {
  std::vector<bench::BenchRow> rows;
  rows.push_back(bench::build_row("PHRASE-5", "local", 5'000'000u, 42, "abc", 10,
                                  true, M(16, 10, 10770000), M(8, 10, 960000)));
  rows.push_back(bench::build_row("PHRASE-5", "local", 5'000'000u, 42, "abc", 10,
                                  true, M(16, 10, 1000000), M(8, 10, 950000)));
  EXPECT_FALSE(bench::all_passed(rows));
}

TEST(ScenarioGate, AllPassedVacuouslyTrueForEmpty) {
  EXPECT_TRUE(bench::all_passed({}));
}

}  // namespace
