// BENCH.7 unit tests -- the reproducible JSONL run-header serializer.
//
// The run-header is the FIRST line of every gated --bench-out JSONL artifact: a
// self-describing record (git rev, corpus seed, scales, surfaces, harness
// version, manifest hash) that lets a CI pipeline replay every golden-metric row
// at the exact code + thresholds it was produced under. These tests pin the
// reproducibility contract on literal RunHeaders with zero filesystem / network:
//   - the record marker + every required key is present,
//   - the same input serializes byte-for-byte identically (cross-commit diffs),
//   - scales serialize as a JSON integer array (empty -> "[]", no trailing comma),
//   - exactly one trailing newline (so it is one valid JSONL line).

#include "run_header.h"

#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

bench::RunHeader sample() {
  bench::RunHeader h;
  h.record = "run_header";
  h.git_rev = "abc1234";
  h.seed = 42;
  h.scales = {150000u, 5000000u};
  h.surfaces = {"local", "oss"};
  h.harness_version = "bench-suite/1";
  h.manifest_hash = "deadbeef";
  return h;
}

std::string serialize(const bench::RunHeader& h) {
  std::ostringstream os;
  bench::write_run_header(os, h);
  return os.str();
}

// header_required_keys -- every reproduction field is present + the record marker.
TEST(RunHeader, HasKeysAndRecordMarker) {
  const std::string line = serialize(sample());
  EXPECT_NE(line.find("\"record\":\"run_header\""), std::string::npos);
  EXPECT_NE(line.find("\"git_rev\":\"abc1234\""), std::string::npos);
  EXPECT_NE(line.find("\"seed\":42"), std::string::npos);
  EXPECT_NE(line.find("\"scales\":"), std::string::npos);
  EXPECT_NE(line.find("\"surfaces\":"), std::string::npos);
  EXPECT_NE(line.find("\"harness_version\":\"bench-suite/1\""), std::string::npos);
  EXPECT_NE(line.find("\"manifest_hash\":\"deadbeef\""), std::string::npos);
}

// header_deterministic -- same input twice yields byte-identical output.
TEST(RunHeader, Deterministic) {
  EXPECT_EQ(serialize(sample()), serialize(sample()));
}

// scales_array_format -- a multi-scale run serializes [150000,5000000].
TEST(RunHeader, ScalesArraySerialized) {
  const std::string line = serialize(sample());
  EXPECT_NE(line.find("\"scales\":[150000,5000000]"), std::string::npos);
  EXPECT_NE(line.find("\"surfaces\":[\"local\",\"oss\"]"), std::string::npos);
}

// empty_scales -- a degenerate config still emits legal JSON ("[]", no comma).
TEST(RunHeader, EmptyScalesIsEmptyArray) {
  bench::RunHeader h = sample();
  h.scales.clear();
  h.surfaces.clear();
  const std::string line = serialize(h);
  EXPECT_NE(line.find("\"scales\":[]"), std::string::npos);
  EXPECT_NE(line.find("\"surfaces\":[]"), std::string::npos);
  EXPECT_EQ(line.find(",]"), std::string::npos);  // no trailing comma
}

// header_first_line_invariant -- exactly one trailing newline, no embedded one.
TEST(RunHeader, SingleTrailingNewline) {
  const std::string line = serialize(sample());
  ASSERT_FALSE(line.empty());
  EXPECT_EQ(line.back(), '\n');
  EXPECT_EQ(line.find('\n'), line.size() - 1);  // the only newline is the last byte
}

// git_rev_dirty_marker -- a dirty working tree is carried through verbatim with
// its -dirty suffix (the serializer must not strip / normalize the marker).
TEST(RunHeader, DirtyGitRevPreserved) {
  bench::RunHeader h = sample();
  h.git_rev = "abc1234-dirty";
  const std::string line = serialize(h);
  EXPECT_NE(line.find("\"git_rev\":\"abc1234-dirty\""), std::string::npos);
}

// The default-constructed record marker is "run_header" so callers cannot forget
// it (the parser contract keys on this marker to find line 1).
TEST(RunHeader, DefaultRecordMarker) {
  bench::RunHeader h;
  EXPECT_EQ(h.record, "run_header");
}

// manifest_hash() is deterministic and stable: the same manifest content hashes
// identically across calls, so the header pins the thresholds the rows were
// gated against.
TEST(RunHeader, ManifestHashDeterministic) {
  EXPECT_EQ(bench::manifest_hash(), bench::manifest_hash());
  EXPECT_FALSE(bench::manifest_hash().empty());
}

}  // namespace
