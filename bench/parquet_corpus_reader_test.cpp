#include "parquet_corpus_reader.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

#include "gtest/gtest.h"

namespace {

// Aborts the test if an arrow::Status is not OK (terse fixture error handling).
#define ARROW_MUST(expr)                                            \
  do {                                                              \
    ::arrow::Status _s = (expr);                                    \
    ASSERT_TRUE(_s.ok()) << #expr << " failed: " << _s.ToString(); \
  } while (0)

// Writes a 2-column parquet (`Body` utf8, `Num` int64) with a small row-group
// size so multi-row-group iteration and cross-group truncation are exercised.
void write_fixture(const std::string& path, const std::vector<std::string>& body,
                   int64_t row_group_size) {
  arrow::StringBuilder body_b;
  arrow::Int64Builder num_b;
  for (size_t i = 0; i < body.size(); ++i) {
    ARROW_MUST(body_b.Append(body[i]));
    ARROW_MUST(num_b.Append(static_cast<int64_t>(i)));
  }
  std::shared_ptr<arrow::Array> body_arr, num_arr;
  ARROW_MUST(body_b.Finish(&body_arr));
  ARROW_MUST(num_b.Finish(&num_arr));

  auto schema = arrow::schema(
      {arrow::field("Body", arrow::utf8()), arrow::field("Num", arrow::int64())});
  auto table = arrow::Table::Make(schema, {body_arr, num_arr});

  std::shared_ptr<arrow::io::FileOutputStream> out;
  auto out_res = arrow::io::FileOutputStream::Open(path);
  ASSERT_TRUE(out_res.ok()) << out_res.status().ToString();
  out = *out_res;
  ARROW_MUST(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out,
                                        row_group_size));
  ARROW_MUST(out->Close());
}

std::string fixture_path(const std::string& name) {
  return ::testing::TempDir() + "/snii_parquet_reader_" + name + ".parquet";
}

const std::vector<std::string>& rows() {
  static const std::vector<std::string> r = {
      "Failed to place order",
      R"({"code":13,"details":"rpc error"})",
      "Checkout",
      "",  // empty body row is preserved so docid == row index
      "trace info warn error fatal",
  };
  return r;
}

TEST(ParquetCorpusReader, ReadsAllRowsInOrderWhenMaxIsZero) {
  const std::string p = fixture_path("all");
  ASSERT_NO_FATAL_FAILURE(write_fixture(p, rows(), /*row_group_size=*/2));
  std::vector<std::string> got = bench::read_text_column(p, "Body", 0);
  EXPECT_EQ(got, rows());
}

TEST(ParquetCorpusReader, MaxDocsTruncatesAcrossRowGroups) {
  const std::string p = fixture_path("trunc");
  ASSERT_NO_FATAL_FAILURE(write_fixture(p, rows(), /*row_group_size=*/2));
  std::vector<std::string> got = bench::read_text_column(p, "Body", 3);
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0], rows()[0]);
  EXPECT_EQ(got[1], rows()[1]);
  EXPECT_EQ(got[2], rows()[2]);
}

TEST(ParquetCorpusReader, MaxDocsLargerThanRowCountReturnsAll) {
  const std::string p = fixture_path("over");
  ASSERT_NO_FATAL_FAILURE(write_fixture(p, rows(), /*row_group_size=*/2));
  std::vector<std::string> got = bench::read_text_column(p, "Body", 9999);
  EXPECT_EQ(got, rows());
}

TEST(ParquetCorpusReader, MissingColumnThrows) {
  const std::string p = fixture_path("missing");
  ASSERT_NO_FATAL_FAILURE(write_fixture(p, rows(), /*row_group_size=*/2));
  EXPECT_THROW(bench::read_text_column(p, "NoSuchColumn", 0), std::runtime_error);
}

TEST(ParquetCorpusReader, OpenFailureThrows) {
  EXPECT_THROW(
      bench::read_text_column("/no/such/dir/missing.parquet", "Body", 0),
      std::runtime_error);
}

}  // namespace
