#pragma once

// BENCH.6 -- real-OSS surface gate (wall-clock latency + golden serial_rounds).
//
// The local cost-model gates (BENCH.1-5) assert deterministic golden metrics
// (serial_rounds / range_gets / request_bytes) byte-for-byte through the SAME
// MeteredFileReader yardstick. This module is the REAL-OSS counterpart: over a
// live object store the wall-clock latency carries per-GET RTT noise, so the gate
// is noise-tolerant on the wall axis (it compares the MEDIAN of repeated samples
// against a documented floor) while staying STRUCTURAL on the axes that are not
// noisy -- serial_rounds (the S3-native round-count claim) must be
// parity-or-better, and the docid sets must match (correctness vetoes latency).
//
// The verdict (a) median wall_ms ratio CL/SNII >= floor AND (b) SNII
// serial_rounds <= CLucene serial_rounds AND (c) docids match. Any failure ->
// overall fail -> nonzero process exit on the --oss path. The aggregation
// (median, ratio) and the verdict are pure and unit-tested on literal sample
// vectors with zero network.

#include <vector>

#include "snii/io/metered_file_reader.h"

namespace bench {

// Median of a wall-clock sample set in ms. Sorts a local copy (callers keep their
// order) and returns v[n/2] -- byte-identical to bench/main.cpp:median() so the
// gated number equals the reported one. Empty input returns 0.0 (no signal).
double median_ms(std::vector<double> samples);

// The structured outcome of the real-OSS surface gate.
struct OssVerdict {
  double wall_ratio_cl_over_snii = 0.0;  // median(cl_wall) / median(sn_wall)
  double min_wall_ratio = 0.0;           // the floor this verdict was gated at
  bool wall_pass = false;    // wall_ratio >= min_wall_ratio (+eps)
  bool rounds_pass = false;  // sn.serial_rounds <= cl.serial_rounds
  bool docids_pass = false;  // the docid-equality correctness gate
  bool overall_pass = false; // wall_pass AND rounds_pass AND docids_pass
};

// Evaluate the real-OSS verdict from repeated wall-clock samples + the two
// engines' (same cost model) IoMetrics + the docid-equality result + the floor.
//
//   wall_ratio = median_ms(cl_wall) / median_ms(sn_wall)
//                -- div-by-zero guard: if median(sn_wall) == 0 the ratio is 1.0
//                   when median(cl_wall) is also 0 (no signal on either side, no
//                   false win) else (double)median(cl_wall) (mirrors main.cpp
//                   ratio()). Both-zero collapses to 1.0, never NaN/inf.
//   wall_pass  = wall_ratio >= min_wall_ratio (inclusive, +1e-9 epsilon so an
//                exact-floor match is a PASS).
//   rounds_pass = sn.serial_rounds <= cl.serial_rounds (parity-or-better; the
//                round count is structural, never noise-relaxed).
//   docids_pass = docids_match (correctness gate; vetoes the verdict).
//   overall_pass = wall_pass AND rounds_pass AND docids_pass.
//
// Pure: no side effects, no I/O, no network.
OssVerdict evaluate_oss(const std::vector<double>& cl_wall,
                        const std::vector<double>& sn_wall,
                        const snii::io::IoMetrics& cl,
                        const snii::io::IoMetrics& sn, bool docids_match,
                        double min_wall_ratio);

}  // namespace bench
