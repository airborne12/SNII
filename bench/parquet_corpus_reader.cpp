#include "parquet_corpus_reader.h"

#include <memory>
#include <stdexcept>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

namespace bench {

namespace {

[[noreturn]] void fail(const std::string& msg) {
  throw std::runtime_error("parquet_corpus_reader: " + msg);
}

// Appends the strings of one chunked column to `out`, stopping once `limit` rows
// total have been collected (limit == 0 means unbounded). Returns true if the
// limit was reached (caller can stop reading further row groups). Accepts both
// utf8 (StringArray) and large_utf8 (LargeStringArray) physical layouts.
bool append_chunked(const arrow::ChunkedArray& col,
                    std::vector<std::string>* out, size_t limit) {
  for (const auto& chunk : col.chunks()) {
    if (auto* s = dynamic_cast<const arrow::StringArray*>(chunk.get())) {
      for (int64_t i = 0; i < s->length(); ++i) {
        out->emplace_back(s->GetString(i));  // null -> empty (data is not null)
        if (limit != 0 && out->size() >= limit) return true;
      }
    } else if (auto* ls =
                   dynamic_cast<const arrow::LargeStringArray*>(chunk.get())) {
      for (int64_t i = 0; i < ls->length(); ++i) {
        out->emplace_back(ls->GetString(i));
        if (limit != 0 && out->size() >= limit) return true;
      }
    } else {
      fail("column is not a UTF-8 string column");
    }
  }
  return false;
}

}  // namespace

std::vector<std::string> read_text_column(const std::string& path,
                                          const std::string& column,
                                          uint32_t max_docs) {
  auto file_res = arrow::io::ReadableFile::Open(path);
  if (!file_res.ok()) fail("cannot open " + path + ": " + file_res.status().ToString());
  std::shared_ptr<arrow::io::RandomAccessFile> file = *file_res;

  std::unique_ptr<parquet::arrow::FileReader> reader;
  arrow::Status st = parquet::arrow::OpenFile(
      file, arrow::default_memory_pool(), &reader);
  if (!st.ok()) fail("cannot read parquet " + path + ": " + st.ToString());

  std::shared_ptr<arrow::Schema> schema;
  st = reader->GetSchema(&schema);
  if (!st.ok()) fail("cannot read schema: " + st.ToString());

  const int col_idx = schema->GetFieldIndex(column);
  if (col_idx < 0) fail("column '" + column + "' not found");
  const arrow::Type::type tid = schema->field(col_idx)->type()->id();
  if (tid != arrow::Type::STRING && tid != arrow::Type::LARGE_STRING) {
    fail("column '" + column + "' is not a string column");
  }

  std::vector<std::string> out;
  const size_t limit = max_docs;  // 0 => unbounded
  if (limit != 0) out.reserve(limit);

  const int n_rg = reader->num_row_groups();
  const std::vector<int> cols = {col_idx};
  for (int rg = 0; rg < n_rg; ++rg) {
    std::shared_ptr<arrow::Table> table;
    st = reader->ReadRowGroup(rg, cols, &table);
    if (!st.ok()) fail("cannot read row group: " + st.ToString());
    if (table->num_columns() != 1) fail("unexpected projected column count");
    if (append_chunked(*table->column(0), &out, limit)) break;
  }
  return out;
}

}  // namespace bench
