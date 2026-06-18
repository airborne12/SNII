#include "snii/writer/snii_compound_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "snii/common/status.h"
#include "snii/format/format_constants.h"
#include "snii/io/local_file.h"
#include "snii/writer/logical_index_writer.h"
#include "snii/writer/spimi_term_buffer.h"

using namespace snii;
using namespace snii::format;
using namespace snii::writer;

namespace {

std::string TempPath() {
  static int counter = 0;
  return "/tmp/snii_stream_test_" + std::to_string(getpid()) + "_" +
         std::to_string(counter++) + ".idx";
}

std::vector<uint8_t> ReadAll(const std::string& path) {
  io::LocalFileReader r;
  EXPECT_TRUE(r.open(path).ok());
  std::vector<uint8_t> out;
  EXPECT_TRUE(r.read_at(0, r.size(), &out).ok());
  return out;
}

// Feeds a deterministic (term, doc, pos) stream into a SPIMI buffer. Docids
// arrive in ascending order per term (the normal tokenizer contract); some
// terms span many docs so both slim and (with enough docs) windowed paths and
// the DICT block splitter are exercised.
void Feed(SpimiTermBuffer* buf, uint32_t doc_count) {
  for (uint32_t d = 0; d < doc_count; ++d) {
    buf->add_token("alpha", d, 0);                 // every doc: high df
    if (d % 2 == 0) buf->add_token("beta", d, 1);  // half the docs
    if (d % 7 == 0) {
      buf->add_token("gamma", d, 2);
      buf->add_token("gamma", d, 5);  // freq 2 in this doc
    }
    if (d == 3 || d == 4) buf->add_token("delta", d, d);  // tiny df
  }
}

// Writes a single-index container from a SniiIndexInput and returns the bytes.
std::vector<uint8_t> WriteContainer(const SniiIndexInput& in) {
  const std::string path = TempPath();
  io::LocalFileWriter writer;
  EXPECT_TRUE(writer.open(path).ok());
  SniiCompoundWriter compound(&writer);
  EXPECT_TRUE(compound.add_logical_index(in).ok());
  EXPECT_TRUE(compound.finish().ok());
  std::vector<uint8_t> bytes = ReadAll(path);
  std::remove(path.c_str());
  return bytes;
}

SniiIndexInput BaseInput(uint32_t doc_count) {
  SniiIndexInput in;
  in.index_id = 1;
  in.index_suffix = "body";
  in.config = IndexConfig::kDocsPositions;
  in.doc_count = doc_count;
  in.target_dict_block_bytes = 512;  // force several DICT blocks
  return in;
}

}  // namespace

// The streaming term_source path must produce a BYTE-IDENTICAL container to the
// materialized terms vector path: the flat-array accumulator + stream-finalize
// must not change a single output byte.
TEST(SpimiStreamingWriter, StreamingMatchesMaterializedBytes) {
  constexpr uint32_t kDocs = 300;

  SpimiTermBuffer mat_buf(/*has_positions=*/true);
  Feed(&mat_buf, kDocs);
  SniiIndexInput mat_in = BaseInput(kDocs);
  mat_in.terms = mat_buf.finalize_sorted();
  const std::vector<uint8_t> mat_bytes = WriteContainer(mat_in);

  SpimiTermBuffer stream_buf(/*has_positions=*/true);
  Feed(&stream_buf, kDocs);
  SniiIndexInput stream_in = BaseInput(kDocs);
  stream_in.term_source = &stream_buf;
  const std::vector<uint8_t> stream_bytes = WriteContainer(stream_in);

  ASSERT_EQ(mat_bytes.size(), stream_bytes.size());
  EXPECT_EQ(mat_bytes, stream_bytes);
}

// The streaming path drains its source: after build the buffer is empty.
TEST(SpimiStreamingWriter, StreamingConsumesSource) {
  SpimiTermBuffer buf(/*has_positions=*/true);
  Feed(&buf, 50);
  EXPECT_GT(buf.unique_terms(), 0u);

  SniiIndexInput in = BaseInput(50);
  in.term_source = &buf;
  LogicalIndexWriter writer(in);
  ASSERT_TRUE(writer.build().ok());
  EXPECT_EQ(buf.unique_terms(), 0u);
}
