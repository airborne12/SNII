#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Streams one string column out of a parquet file into a flat per-document text
// vector (row order == docid), so the real OTel-log dataset can drive the full
// E2E pipeline (tokenize -> build -> compare) without a pre-export step. Reads
// row group by row group and stops as soon as `max_docs` rows are collected, so
// a multi-billion-row file is never fully materialized.
namespace bench {

// Reads up to `max_docs` rows of the string column `column` from the parquet
// file at `path`; each row becomes one document. `max_docs == 0` reads every
// row. Throws std::runtime_error if the file cannot be opened, `column` is
// absent, or `column` is not a UTF-8 / large-UTF-8 string column.
std::vector<std::string> read_text_column(const std::string& path,
                                          const std::string& column,
                                          uint32_t max_docs);

}  // namespace bench
