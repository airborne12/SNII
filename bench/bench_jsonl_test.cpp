// Unit tests for the JSONL result emitter (bench::write_jsonl / json_escape).
//
// These are I/O-free (in-memory ostringstream), CLucene-free, corpus-free and
// build-free: they feed a hand-built BenchRow into write_jsonl and assert the
// emitted line carries every required reproduction key, that large uint64 byte
// counts keep integer fidelity (no scientific notation), that empty verdicts emit
// a legal empty array, and that quotes/backslashes are escaped. This is BENCH.3:
// the per-row JSONL artifact that makes each golden-metric measurement
// reproducible and CI-gateable.

#include "bench_jsonl.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>

#include "metric_gate.h"
#include "snii/io/metered_file_reader.h"

namespace {

// Build an IoMetrics with the three golden metrics the emitter surfaces.
snii::io::IoMetrics M(uint64_t serial_rounds, uint64_t range_gets,
                      uint64_t total_request_bytes) {
  snii::io::IoMetrics m;
  m.serial_rounds = serial_rounds;
  m.range_gets = range_gets;
  m.total_request_bytes = total_request_bytes;
  return m;
}

// A representative healthy row used by several tests.
bench::BenchRow HealthyRow() {
  bench::BenchRow r;
  r.scenario_id = "PHRASE-5";
  r.surface = "local";
  r.doc_count = 5000000u;
  r.seed = 42u;
  r.git_rev = "abc1234";
  r.hits = 10u;
  r.docids_match = true;
  r.clucene = M(21, 10, 10770000);
  r.snii = M(5, 10, 960000);
  r.verdicts.push_back(bench::evaluate_metric(
      r.clucene, r.snii,
      {"serial_rounds", bench::GateCmp::kLeAbsolute, 0, 8}));
  r.overall_pass = true;
  return r;
}

// Count occurrences of needle in haystack.
size_t Count(const std::string& hay, const std::string& needle) {
  size_t n = 0, pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    ++n;
    pos += needle.size();
  }
  return n;
}

// --- required reproduction keys + exactly one trailing newline ---

TEST(Jsonl, HasRequiredKeys) {
  std::ostringstream os;
  bench::write_jsonl(os, HealthyRow());
  const std::string s = os.str();
  for (const char* key :
       {"\"scenario_id\"", "\"surface\"", "\"doc_count\"", "\"seed\"",
        "\"git_rev\"", "\"hits\"", "\"docids_match\"", "\"clucene\"",
        "\"snii\"", "\"serial_rounds\"", "\"range_gets\"",
        "\"total_request_bytes\"", "\"verdicts\"", "\"overall_pass\""}) {
    EXPECT_NE(s.find(key), std::string::npos) << "missing key " << key;
  }
  // exactly one trailing newline, none embedded.
  ASSERT_FALSE(s.empty());
  EXPECT_EQ(s.back(), '\n');
  EXPECT_EQ(Count(s, "\n"), 1u);
}

// --- large uint64 must be an integer literal, never 1.39e8 ---

TEST(Jsonl, LargeBytesNoSciNotation) {
  bench::BenchRow r = HealthyRow();
  r.verdicts.clear();  // verdict.ratio is the only legitimate float; drop it here
  r.snii.total_request_bytes = 139377956ull;
  std::ostringstream os;
  bench::write_jsonl(os, r);
  const std::string s = os.str();
  EXPECT_NE(s.find("139377956"), std::string::npos);
  EXPECT_EQ(s.find("1.39e"), std::string::npos);
  EXPECT_EQ(s.find("e+0"), std::string::npos);
  // With verdicts dropped, no integer metric field is rendered as a float.
  EXPECT_EQ(s.find('.'), std::string::npos);
}

// --- empty verdicts -> legal empty array, not a missing key/trailing comma ---

TEST(Jsonl, EmptyVerdictsEmitsEmptyArray) {
  bench::BenchRow r = HealthyRow();
  r.verdicts.clear();
  std::ostringstream os;
  bench::write_jsonl(os, r);
  EXPECT_NE(os.str().find("\"verdicts\":[]"), std::string::npos);
}

// --- quote and backslash escaping ---

TEST(Jsonl, EscapesQuotesAndBackslash) {
  EXPECT_EQ(bench::json_escape("a\"b\\c"), "a\\\"b\\\\c");
}

TEST(Jsonl, EscapesEmbeddedNewline) {
  // a stray newline inside a string field must not split the JSONL line.
  EXPECT_EQ(bench::json_escape("a\nb"), "a\\nb");
}

// --- a stray quote/backslash in a string field stays one line, escaped ---

TEST(Jsonl, StringFieldsAreEscapedInRow) {
  bench::BenchRow r = HealthyRow();
  r.git_rev = "a\"b\\c";
  std::ostringstream os;
  bench::write_jsonl(os, r);
  const std::string s = os.str();
  EXPECT_NE(s.find("a\\\"b\\\\c"), std::string::npos);
  EXPECT_EQ(Count(s, "\n"), 1u);  // still exactly one line
}

// --- zero metrics row: zeros are a legal measurement, not "missing" ---

TEST(Jsonl, ZeroMetricsRow) {
  bench::BenchRow r;
  r.scenario_id = "MATCH-ALL";
  r.surface = "local";
  r.doc_count = 2000u;
  r.seed = 7u;
  r.git_rev = "deadbee";
  r.hits = 2000u;
  r.docids_match = true;
  r.clucene = M(0, 0, 0);
  r.snii = M(0, 0, 0);
  r.overall_pass = true;  // docid gate alone
  std::ostringstream os;
  bench::write_jsonl(os, r);
  const std::string s = os.str();
  EXPECT_NE(s.find("\"serial_rounds\":0"), std::string::npos);
  EXPECT_NE(s.find("\"total_request_bytes\":0"), std::string::npos);
  EXPECT_NE(s.find("\"overall_pass\":true"), std::string::npos);
}

// --- docid mismatch must serialize false/false and be visible ---

TEST(Jsonl, DocidsMismatchOverallFail) {
  bench::BenchRow r = HealthyRow();
  r.docids_match = false;
  r.overall_pass = false;
  std::ostringstream os;
  bench::write_jsonl(os, r);
  const std::string s = os.str();
  EXPECT_NE(s.find("\"docids_match\":false"), std::string::npos);
  EXPECT_NE(s.find("\"overall_pass\":false"), std::string::npos);
}

// --- hits is a size_t and must print as a plain integer (no %zu truncation) ---

TEST(Jsonl, HitsSizeTMax) {
  bench::BenchRow r = HealthyRow();
  r.hits = 5000000u;
  std::ostringstream os;
  bench::write_jsonl(os, r);
  EXPECT_NE(os.str().find("\"hits\":5000000"), std::string::npos);
}

// --- verdicts array carries the per-metric clucene/snii/pass triplet ---

TEST(Jsonl, VerdictArrayCarriesMetricFields) {
  std::ostringstream os;
  bench::write_jsonl(os, HealthyRow());
  const std::string s = os.str();
  // the single serial_rounds verdict: clucene 21, snii 5, pass true.
  EXPECT_NE(s.find("\"metric\":\"serial_rounds\""), std::string::npos);
  EXPECT_NE(s.find("\"clucene\":21"), std::string::npos);
  EXPECT_NE(s.find("\"snii\":5"), std::string::npos);
  EXPECT_NE(s.find("\"pass\":true"), std::string::npos);
}

}  // namespace
