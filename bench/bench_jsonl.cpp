// BENCH.3 -- JSONL result emitter implementation.
//
// Pure serializer: one valid JSON object + one trailing '\n' per BenchRow. All
// metric values are written as integer literals (PRIu64 / %zu) so a CI diff sees
// exact byte counts (139377956, never 1.39e8). No floats are ever emitted.

#include "bench_jsonl.h"

#include <cinttypes>
#include <cstdio>

namespace bench {

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        // Other control chars (< 0x20) are forbidden bare in JSON: emit \u00XX
        // so a stray byte can never break the line. Printable bytes pass through.
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned int>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

namespace {

// "key":"escaped-string-value" (no leading comma; caller controls separators).
void key_str(std::ostream& os, const char* key, const std::string& value) {
  os << '"' << key << "\":\"" << json_escape(value) << '"';
}

// "key":<uint64 integer literal>
void key_u64(std::ostream& os, const char* key, uint64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%" PRIu64, value);
  os << '"' << key << "\":" << buf;
}

// "key":<size_t integer literal>
void key_size(std::ostream& os, const char* key, size_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%zu", value);
  os << '"' << key << "\":" << buf;
}

// "key":true|false
void key_bool(std::ostream& os, const char* key, bool value) {
  os << '"' << key << "\":" << (value ? "true" : "false");
}

// {"serial_rounds":N,"range_gets":N,"total_request_bytes":N} -- the three golden
// metrics only, all as integer literals.
void write_metrics(std::ostream& os, const snii::io::IoMetrics& m) {
  os << '{';
  key_u64(os, "serial_rounds", m.serial_rounds);
  os << ',';
  key_u64(os, "range_gets", m.range_gets);
  os << ',';
  key_u64(os, "total_request_bytes", m.total_request_bytes);
  os << '}';
}

// One verdict object: {"metric":...,"clucene":N,"snii":N,"ratio":R,"pass":b}.
// ratio is the one float in the row (advantage ratio is inherently fractional),
// emitted with %.6g so it is human-readable and never collides with the integer
// metric fields a CI diff compares.
void write_verdict(std::ostream& os, const MetricVerdict& v) {
  os << '{';
  key_str(os, "metric", v.metric_name);
  os << ',';
  key_u64(os, "clucene", v.clucene);
  os << ',';
  key_u64(os, "snii", v.snii);
  os << ',';
  char rbuf[32];
  std::snprintf(rbuf, sizeof(rbuf), "%.6g", v.ratio);
  os << "\"ratio\":" << rbuf << ',';
  key_bool(os, "pass", v.pass);
  os << ',';
  key_str(os, "reason", v.reason);
  os << '}';
}

}  // namespace

void write_jsonl(std::ostream& os, const BenchRow& row) {
  os << '{';
  key_str(os, "scenario_id", row.scenario_id);
  os << ',';
  key_str(os, "surface", row.surface);
  os << ',';
  key_u64(os, "doc_count", static_cast<uint64_t>(row.doc_count));
  os << ',';
  key_u64(os, "seed", row.seed);
  os << ',';
  key_str(os, "git_rev", row.git_rev);
  os << ',';
  key_size(os, "hits", row.hits);
  os << ',';
  key_bool(os, "docids_match", row.docids_match);
  os << ',';
  os << "\"clucene\":";
  write_metrics(os, row.clucene);
  os << ',';
  os << "\"snii\":";
  write_metrics(os, row.snii);
  os << ',';
  os << "\"verdicts\":[";
  for (size_t i = 0; i < row.verdicts.size(); ++i) {
    if (i != 0) os << ',';
    write_verdict(os, row.verdicts[i]);
  }
  os << ']';
  os << ',';
  key_bool(os, "overall_pass", row.overall_pass);
  os << "}\n";
}

}  // namespace bench
