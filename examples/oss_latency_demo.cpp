// Real-OSS wall-clock demo for the SNII read path: build an index, upload it to
// OSS, then run queries over an S3FileReader and measure actual latency.
//
// Standalone example (NOT part of any CMake target, since it has its own main and
// needs aws). Build the S3-enabled lib first, then compile against it + bench
// corpus_gen + the aws archives:
//   cmake -S . -B build-s3 -DSNII_WITH_S3=ON && cmake --build build-s3 -j
//   g++ -std=c++20 -DSNII_WITH_S3 -I core/include -I bench \
//       -I <doris-thirdparty-install>/include examples/oss_latency_demo.cpp \
//       bench/corpus_gen.cpp build-s3/core/libsnii.a \
//       -Wl,--start-group <aws + libcurl/ssl/crypto/zstd/roaring archives> -Wl,--end-group \
//       -lpthread -ldl -lm -lz -lrt -lresolv -o oss_demo
//   SNII_OSS_AK=... SNII_OSS_SK=... ./oss_demo 1000000
// (See cmake/FindAwsSdk.cmake for the exact archive list / link order.)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "corpus_gen.h"
#include "snii/io/local_file.h"
#include "snii/io/metered_file_reader.h"
#include "snii/io/s3_object_store.h"
#include "snii/query/bm25_scorer.h"
#include "snii/query/phrase_query.h"
#include "snii/query/term_query.h"
#include "snii/reader/snii_segment_reader.h"
#include "snii/writer/snii_compound_writer.h"
#include "snii/writer/spimi_term_buffer.h"

using namespace snii;

static std::vector<uint8_t> ReadWholeFile(const std::string& path) {
  io::LocalFileReader r;
  if (!r.open(path).ok()) return {};
  std::vector<uint8_t> buf;
  r.read_at(0, r.size(), &buf);
  return buf;
}

static double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv) {
  const uint32_t docs = (argc > 1) ? std::strtoul(argv[1], nullptr, 10) : 200000;
  const char* ak = std::getenv("SNII_OSS_AK");
  const char* sk = std::getenv("SNII_OSS_SK");
  if (!ak || !sk) { std::fprintf(stderr, "set SNII_OSS_AK / SNII_OSS_SK\n"); return 2; }
  io::AwsApiGuard aws_guard;  // Aws::InitAPI for the whole program lifetime.

  // 1. Corpus + SNII index to a local file.
  const bench::Corpus corpus = bench::generate(docs, docs / 4, 1.1, 12, 42);
  writer::SpimiTermBuffer buf(true);
  for (uint32_t d = 0; d < corpus.doc_count; ++d)
    for (uint32_t k = 0; k < corpus.docs[d].size(); ++k)
      buf.add_token(corpus.vocab[corpus.docs[d][k]], d, k);
  writer::SniiIndexInput in;
  in.index_id = 1; in.index_suffix = "body";
  in.config = format::IndexConfig::kDocsPositions;
  in.doc_count = corpus.doc_count;
  in.terms = buf.finalize_sorted();
  const std::string local = "/tmp/snii_oss_demo.idx";
  { io::LocalFileWriter w; w.open(local); writer::SniiCompoundWriter cw(&w);
    if (!cw.add_logical_index(in).ok() || !cw.finish().ok()) { std::fprintf(stderr, "build failed\n"); return 1; } }
  const std::vector<uint8_t> idx_bytes = ReadWholeFile(local);
  std::printf("SNII index: docs=%u size=%zu bytes\n", docs, idx_bytes.size());

  // 2. Upload to OSS.
  io::S3Config cfg; cfg.endpoint = "oss-cn-hongkong.aliyuncs.com"; cfg.region = "cn-hongkong";
  cfg.bucket = "doris-community-test"; cfg.prefix = "cloud_regression/snii_oss_demo";
  cfg.ak = ak; cfg.sk = sk;
  const std::string key = "demo_" + std::to_string(docs) + ".idx";
  { io::S3FileWriter w; if (!w.open(cfg, key).ok()) { std::fprintf(stderr, "s3 open failed\n"); return 1; }
    auto t0 = std::chrono::steady_clock::now();
    w.append(Slice(idx_bytes)); if (!w.finalize().ok()) { std::fprintf(stderr, "upload failed\n"); return 1; }
    std::printf("uploaded to oss://%s/%s/%s in %.0f ms\n", cfg.bucket.c_str(), cfg.prefix.c_str(), key.c_str(), ms_since(t0)); }

  // 3. Open over S3FileReader (+ metered cost model) and query.
  io::S3FileReader s3r;
  if (!io::S3FileReader::open(cfg, key, &s3r).ok()) { std::fprintf(stderr, "s3 read open failed\n"); return 1; }
  io::MeteredFileReader metered(&s3r);
  reader::SniiSegmentReader seg;
  if (!reader::SniiSegmentReader::open(&metered, &seg).ok()) { std::fprintf(stderr, "seg open failed\n"); return 1; }
  reader::LogicalIndexReader idx;
  seg.open_index(1, "body", &idx);

  const std::string hi = corpus.vocab[bench::highest_df_term(corpus)];
  const std::vector<std::string> phrase = bench::extract_phrase(corpus, 5);

  std::printf("\n=== real-OSS query latency (over S3FileReader) ===\n");
  { metered.reset_metrics(); auto t0 = std::chrono::steady_clock::now();
    std::vector<uint32_t> d; query::term_query(idx, hi, &d);
    std::printf("TERM  '%s' hits=%zu  wall=%.0f ms  serial_rounds=%llu range_gets=%llu remote_bytes=%llu\n",
      hi.c_str(), d.size(), ms_since(t0),
      (unsigned long long)metered.metrics().serial_rounds,
      (unsigned long long)metered.metrics().range_gets,
      (unsigned long long)metered.metrics().remote_bytes); }
  { metered.reset_metrics(); auto t0 = std::chrono::steady_clock::now();
    std::vector<uint32_t> d; query::phrase_query(idx, phrase, &d);
    std::printf("PHRASE 5-term hits=%zu  wall=%.0f ms  serial_rounds=%llu range_gets=%llu remote_bytes=%llu\n",
      d.size(), ms_since(t0),
      (unsigned long long)metered.metrics().serial_rounds,
      (unsigned long long)metered.metrics().range_gets,
      (unsigned long long)metered.metrics().remote_bytes); }
  return 0;
}
