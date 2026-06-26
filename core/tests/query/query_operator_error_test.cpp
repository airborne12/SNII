#include "snii/query/boolean_query.h"
#include "snii/query/phrase_query.h"
#include "snii/query/prefix_query.h"
#include "snii/query/regexp_query.h"
#include "snii/query/term_query.h"
#include "snii/query/wildcard_query.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "snii/common/status.h"
#include "snii/io/file_reader.h"
#include "snii/io/local_file.h"
#include "snii/reader/logical_index_reader.h"
#include "snii/reader/snii_segment_reader.h"
#include "snii/writer/snii_compound_writer.h"
#include "snii/writer/spimi_term_buffer.h"

using namespace snii;
using namespace snii::reader;
using namespace snii::writer;

namespace {

std::string TempPath() {
  static int counter = 0;
  return "/tmp/snii_query_operator_error_" + std::to_string(getpid()) + "_" +
         std::to_string(counter++) + ".idx";
}

class FaultInjectingReader : public io::FileReader {
 public:
  explicit FaultInjectingReader(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

  void set_fail_reads(bool v) { fail_reads_ = v; }

  Status read_at(uint64_t offset, size_t len, std::vector<uint8_t>* out) override {
    if (fail_reads_) return Status::IoError("injected read failure");
    if (out == nullptr) return Status::InvalidArgument("memory reader null out");
    if (offset > bytes_.size() || len > bytes_.size() - offset) {
      return Status::Corruption("memory reader read past end");
    }
    out->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset + len));
    return Status::OK();
  }

  uint64_t size() const override { return bytes_.size(); }

 private:
  std::vector<uint8_t> bytes_;
  bool fail_reads_ = false;
};

struct Corpus {
  std::vector<std::vector<std::string>> docs = {
      {"alpha", "beta", "gamma"},
      {"alpha", "beta", "delta"},
      {"alpha", "bravo", "gamma"},
      {"beta", "gamma"},
      {"alphabeta", "beta"},
  };
};

void BuildIndexBytes(const Corpus& corpus, snii::format::IndexConfig config,
                     std::vector<uint8_t>* bytes) {
  SpimiTermBuffer buf(/*has_positions=*/config != snii::format::IndexConfig::kDocsOnly);
  for (uint32_t d = 0; d < corpus.docs.size(); ++d) {
    const std::vector<std::string>& terms = corpus.docs[d];
    for (uint32_t pos = 0; pos < terms.size(); ++pos) {
      buf.add_token(terms[pos], d, pos);
    }
  }

  SniiIndexInput in;
  in.index_id = 1;
  in.index_suffix = "body";
  in.config = config;
  in.doc_count = static_cast<uint32_t>(corpus.docs.size());
  if (config == snii::format::IndexConfig::kDocsPositionsScoring) {
    in.encoded_norms.assign(corpus.docs.size(), 1);
  }
  in.terms = buf.finalize_sorted();
  in.target_dict_block_bytes = 512;

  const std::string path = TempPath();
  {
    io::LocalFileWriter writer;
    ASSERT_TRUE(writer.open(path).ok());
    SniiCompoundWriter compound(&writer);
    ASSERT_TRUE(compound.add_logical_index(in).ok());
    ASSERT_TRUE(compound.finish().ok());
  }
  {
    io::LocalFileReader file;
    ASSERT_TRUE(file.open(path).ok());
    ASSERT_TRUE(file.read_at(0, file.size(), bytes).ok());
  }
  std::remove(path.c_str());
}

LogicalIndexReader OpenIndex(FaultInjectingReader* file, SniiSegmentReader* segment) {
  EXPECT_TRUE(SniiSegmentReader::open(file, segment).ok());
  LogicalIndexReader idx;
  EXPECT_TRUE(segment->open_index(1, "body", &idx).ok());
  return idx;
}

void ExpectIoError(const Status& status) {
  EXPECT_EQ(status.code(), StatusCode::kIoError) << status.to_string();
}

}  // namespace

TEST(QueryOperatorBoundaries, RejectNullOutputPointers) {
  LogicalIndexReader idx;
  auto* null_docs = static_cast<std::vector<uint32_t>*>(nullptr);
  EXPECT_EQ(query::term_query(idx, "alpha", nullptr).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(query::boolean_or(idx, {"alpha"}, null_docs).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(query::boolean_and(idx, {"alpha"}, nullptr).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(query::prefix_query(idx, "al", null_docs).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(query::wildcard_query(idx, "a*", null_docs).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(query::regexp_query(idx, "a.*", null_docs).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(query::phrase_query(idx, {"alpha"}, nullptr).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(query::phrase_prefix_query(idx, {"alpha"}, nullptr).code(),
            StatusCode::kInvalidArgument);
}

TEST(QueryOperatorBoundaries, EmptyMissingAndSingleTermCasesAreWellDefined) {
  std::vector<uint8_t> bytes;
  BuildIndexBytes(Corpus{}, snii::format::IndexConfig::kDocsPositionsScoring, &bytes);
  FaultInjectingReader file(std::move(bytes));
  SniiSegmentReader segment;
  LogicalIndexReader idx = OpenIndex(&file, &segment);

  std::vector<uint32_t> got = {99};
  ASSERT_TRUE(query::term_query(idx, "missing", &got).ok());
  EXPECT_TRUE(got.empty());

  got = {99};
  ASSERT_TRUE(query::boolean_or(idx, {}, &got).ok());
  EXPECT_TRUE(got.empty());

  got = {99};
  ASSERT_TRUE(query::boolean_and(idx, {}, &got).ok());
  EXPECT_TRUE(got.empty());

  got = {99};
  ASSERT_TRUE(query::boolean_and(idx, {"alpha", "missing"}, &got).ok());
  EXPECT_TRUE(got.empty());

  got = {99};
  ASSERT_TRUE(query::phrase_query(idx, {}, &got).ok());
  EXPECT_TRUE(got.empty());

  got = {99};
  ASSERT_TRUE(query::phrase_prefix_query(idx, {}, &got).ok());
  EXPECT_TRUE(got.empty());

  ASSERT_TRUE(query::prefix_query(idx, "", &got).ok());
  EXPECT_EQ(got, (std::vector<uint32_t>{0, 1, 2, 3, 4}));

  ASSERT_TRUE(query::phrase_query(idx, {"alpha"}, &got).ok());
  EXPECT_EQ(got, (std::vector<uint32_t>{0, 1, 2}));

  ASSERT_TRUE(query::phrase_prefix_query(idx, {"alpha"}, &got).ok());
  EXPECT_EQ(got, (std::vector<uint32_t>{0, 1, 2, 4}));

  ASSERT_TRUE(query::wildcard_query(idx, "", &got).ok());
  EXPECT_TRUE(got.empty());

  EXPECT_EQ(query::regexp_query(idx, "[", &got).code(), StatusCode::kInvalidArgument);
}

TEST(QueryOperatorBoundaries, PositionQueriesRejectDocsOnlyIndex) {
  std::vector<uint8_t> bytes;
  BuildIndexBytes(Corpus{}, snii::format::IndexConfig::kDocsOnly, &bytes);
  FaultInjectingReader file(std::move(bytes));
  SniiSegmentReader segment;
  LogicalIndexReader idx = OpenIndex(&file, &segment);

  std::vector<uint32_t> got;
  EXPECT_EQ(query::phrase_query(idx, {"alpha", "beta"}, &got).code(),
            StatusCode::kUnsupported);
  EXPECT_EQ(query::phrase_prefix_query(idx, {"alpha", "b"}, &got).code(),
            StatusCode::kUnsupported);
}

TEST(QueryOperatorIoErrors, PropagateUnderlyingReadFailures) {
  std::vector<uint8_t> bytes;
  BuildIndexBytes(Corpus{}, snii::format::IndexConfig::kDocsPositionsScoring, &bytes);
  FaultInjectingReader file(std::move(bytes));
  SniiSegmentReader segment;
  LogicalIndexReader idx = OpenIndex(&file, &segment);
  file.set_fail_reads(true);

  std::vector<uint32_t> got;
  ExpectIoError(query::term_query(idx, "alpha", &got));
  ExpectIoError(query::boolean_or(idx, {"alpha", "beta"}, &got));
  ExpectIoError(query::boolean_and(idx, {"alpha", "beta"}, &got));
  ExpectIoError(query::prefix_query(idx, "al", &got));
  ExpectIoError(query::wildcard_query(idx, "alpha*", &got));
  ExpectIoError(query::regexp_query(idx, "alpha.*", &got));
  ExpectIoError(query::phrase_query(idx, {"alpha", "beta"}, &got));
  ExpectIoError(query::phrase_prefix_query(idx, {"alpha", "b"}, &got));
}
