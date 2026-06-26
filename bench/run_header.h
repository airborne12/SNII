#pragma once

// BENCH.7 -- reproducible run-header + manifest fingerprint.
//
// The run-header is the FIRST JSONL line of every gated --bench-out artifact. It
// makes the benchmark SUITE a single self-describing, replayable artifact: given
// the same git rev + corpus seed + declared thresholds (manifest hash), a CI
// pipeline can re-run the suite and reproduce every golden-metric row, and can
// diff the JSONL across commits keyed by this header. The serializer is pure:
// no I/O, no CLucene, no corpus, no build -- it takes a fully populated RunHeader
// and writes exactly one line of valid JSON terminated by a single newline.
//
// Companion to bench_jsonl.h (the per-row emitter): the header describes the run,
// each subsequent line describes one gated scenario row. The top-level driver
// (bench/run_suite.sh) executes the gated mode at each scale and aggregates exit
// codes; the header lets the resulting JSONL stand alone for cross-commit diffs.

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace bench {

// The self-describing run-header. `record` is the fixed marker a parser keys on
// to recognize line 1 ("run_header"); the remaining fields are the reproduction
// metadata that pins every following row to the exact code + thresholds it was
// produced under. `scales` are the corpus doc-counts the suite was run at (e.g.
// {150000, 5000000}); `surfaces` are the measurement surfaces ("local", "oss").
struct RunHeader {
  std::string record = "run_header";  // fixed first-line marker
  std::string git_rev;                // git rev (may carry a "-dirty" suffix)
  uint64_t seed = 0;                  // corpus_gen seed (reproducibility)
  std::vector<uint32_t> scales;       // corpus doc-counts the suite ran at
  std::vector<std::string> surfaces;  // measurement surfaces ("local"/"oss")
  std::string harness_version;        // bench-suite harness version string
  std::string manifest_hash;          // fingerprint of the declared thresholds
};

// Write exactly one JSON object followed by exactly one trailing '\n'. `seed` is
// an integer literal (full uint64 fidelity for a CI diff). `scales` serializes as
// a JSON integer array ("[]" when empty, no trailing comma); `surfaces` as a JSON
// string array. String fields are JSON-escaped (reusing bench_jsonl's json_escape)
// so a stray quote/newline can never split the record. Deterministic: the same
// RunHeader serializes byte-for-byte identically.
void write_run_header(std::ostream& os, const RunHeader& h);

// A deterministic fingerprint of the declarative threshold manifest (BENCH.2):
// the canonical (scenario_id, scale, surface) -> declared thresholds mapping,
// FNV-1a hashed. Stable across calls and processes; changes iff a declared
// threshold changes -- so a header's manifest_hash pins the gate the rows were
// judged against. Pure: no I/O.
std::string manifest_hash();

}  // namespace bench
