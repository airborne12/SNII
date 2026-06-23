// Demo: how Doris's InvertedIndexColumnWriter would drive SNII to write one segment's
// inverted index. Mirrors the Doris interface (init / add_values / add_nulls / finish)
// and calls the REAL SNII writer API. The analyzer and the MemTracker bridge are Doris
// concerns (stubbed here with a clear marker); everything else is the actual SNII flow.
//
// Build & run:
//   cmake -S . -B build -DSNII_BUILD_EXAMPLES=ON && cmake --build build --target doris_writer_demo
//   ./build/examples/doris_writer_demo

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "snii/format/format_constants.h"
#include "snii/io/local_file.h"
#include "snii/query/bm25_scorer.h"   // encode_norm
#include "snii/query/term_query.h"    // self-check
#include "snii/reader/logical_index_reader.h"
#include "snii/reader/snii_segment_reader.h"
#include "snii/writer/logical_index_writer.h"  // SniiIndexInput
#include "snii/writer/memory_reporter.h"
#include "snii/writer/snii_compound_writer.h"
#include "snii/writer/spimi_term_buffer.h"

namespace snii_doris_demo {

using snii::format::IndexConfig;
using snii::writer::MemoryReporter;
using snii::writer::SniiCompoundWriter;
using snii::writer::SniiIndexInput;
using snii::writer::SpimiTermBuffer;

// ---- Doris-side stand-ins (NOT SNII) ---------------------------------------

// In real Doris this is the field's configured analyzer; here a whitespace splitter.
std::vector<std::string> analyze(std::string_view value) {
  std::vector<std::string> tokens;
  std::istringstream ss{std::string(value)};
  for (std::string t; ss >> t;) tokens.push_back(std::move(t));
  return tokens;
}

// In real Doris this calls the load task's MemTrackerLimiter consume/release so the
// inverted-index RAM is counted by MemTableMemoryLimiter's soft/hard limit (gate 1).
MemoryReporter::ConsumeReleaseFn make_mem_tracker_bridge() {
  return [](int64_t delta) {
    // doris::thread_context()->thread_mem_tracker_mgr->consume(delta);  // delta>0
    // (release is a negative consume; null off-Doris). No-op in the demo.
    (void)delta;
  };
}

// ---- The SNII-backed column writer (mirrors Doris InvertedIndexColumnWriter) ----

class SniiInvertedIndexColumnWriter {
 public:
  // gate-2 unified cap = Doris's inverted-index buffer config (e.g. 512 MiB). 0=unlimited.
  SniiInvertedIndexColumnWriter(std::string idx_path, std::string field,
                                IndexConfig config, uint64_t mem_cap_bytes)
      : path_(std::move(idx_path)),
        field_(std::move(field)),
        config_(config),
        reporter_(make_mem_tracker_bridge(), mem_cap_bytes),
        buf_(/*has_positions=*/snii::format::has_positions(config),
             /*spill_threshold_bytes=*/0,  // unified cap lives in the reporter
             &reporter_) {}

  // Doris: add_values(field, values, count) -- one logical doc (row) per value.
  void add_values(const std::vector<std::string>& values) {
    for (const std::string& v : values) {
      uint32_t pos = 0;
      for (const std::string& tok : analyze(v)) {     // Doris analyzer
        buf_.add_token(tok, doc_id_, pos++);          // <-- SNII: string-keyed add
      }
      if (config_ == IndexConfig::kDocsPositionsScoring) {
        doc_len_.push_back(pos);                      // token count = doc length
      }
      ++doc_id_;
    }
  }

  // Doris: add_nulls(count) -- null rows advance the docid, emit no tokens.
  void add_nulls(uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      if (config_ == IndexConfig::kDocsPositionsScoring) doc_len_.push_back(0);
      ++doc_id_;
    }
  }

  // Doris: finish() -- drain + k-way merge (gate-2 temp segments) -> the .idx.
  snii::Status finish() {
    if (snii::Status s = buf_.status(); !s.ok()) return s;  // surface latched feed errors

    SniiIndexInput in;
    in.index_id = 1;
    in.index_suffix = field_;
    in.config = config_;
    in.doc_count = doc_id_;
    in.term_source = &buf_;          // STREAM the buffer (one term materialized at a time)
    in.mem_reporter = &reporter_;    // same reporter -> unified gate-2 cap across buffers
    if (config_ == IndexConfig::kDocsPositionsScoring) {
      in.encoded_norms.resize(doc_len_.size());
      for (size_t d = 0; d < doc_len_.size(); ++d)
        in.encoded_norms[d] = snii::query::encode_norm(doc_len_[d]);  // <-- SNII BM25 norm
    }

    snii::io::LocalFileWriter out;
    if (snii::Status s = out.open(path_); !s.ok()) return s;
    SniiCompoundWriter compound(&out);                 // per-segment container
    if (snii::Status s = compound.add_logical_index(in); !s.ok()) return s;  // <-- build+merge
    return compound.finish();                          // <-- write .idx
  }

 private:
  std::string path_;
  std::string field_;
  IndexConfig config_;
  MemoryReporter reporter_;   // declared BEFORE buf_ so it outlives it
  SpimiTermBuffer buf_;
  uint32_t doc_id_ = 0;
  std::vector<uint32_t> doc_len_;
};

}  // namespace snii_doris_demo

// ---- Drive it + self-verify by reading the .idx back -----------------------

int main() {
  using namespace snii_doris_demo;
  const std::string path = "/tmp/snii_doris_demo.idx";

  SniiInvertedIndexColumnWriter w(path, "body", IndexConfig::kDocsPositions,
                                  /*mem_cap=*/512ull * 1024 * 1024);
  // Three rows fed exactly as Doris's segment writer would (add_values per batch).
  w.add_values({"the quick brown fox", "the lazy dog"});
  w.add_nulls(1);                              // row 2 is NULL
  w.add_values({"quick brown dog"});           // row 3
  if (snii::Status s = w.finish(); !s.ok()) {
    std::fprintf(stderr, "finish failed: %s\n", s.to_string().c_str());
    return 1;
  }

  // Read the produced .idx back and query a term to prove it round-trips.
  snii::io::LocalFileReader r;
  if (!r.open(path).ok()) { std::fprintf(stderr, "open .idx failed\n"); return 1; }
  snii::reader::SniiSegmentReader seg;
  if (!snii::reader::SniiSegmentReader::open(&r, &seg).ok()) return 1;
  snii::reader::LogicalIndexReader idx;
  if (!seg.open_index(1, "body", &idx).ok()) return 1;

  for (const char* term : {"quick", "dog", "fox", "absent"}) {
    std::vector<uint32_t> docs;
    snii::query::term_query(idx, term, &docs);
    std::printf("term '%s' -> docids [", term);
    for (uint32_t d : docs) std::printf("%u ", d);
    std::printf("]\n");
  }
  std::printf("OK: wrote + read back %s\n", path.c_str());
  return 0;
}
