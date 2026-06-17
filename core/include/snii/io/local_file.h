#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/io/file_reader.h"
#include "snii/io/file_writer.h"

namespace snii::io {

// Local-filesystem FileReader. Uses pread for positional, thread-safe reads
// (so concurrent batch fetches do not contend on a shared file offset).
class LocalFileReader : public FileReader {
 public:
  LocalFileReader() = default;
  ~LocalFileReader() override;

  LocalFileReader(const LocalFileReader&) = delete;
  LocalFileReader& operator=(const LocalFileReader&) = delete;

  Status open(const std::string& path);
  Status read_at(uint64_t offset, size_t len, std::vector<uint8_t>* out) override;
  uint64_t size() const override { return size_; }

 private:
  int fd_ = -1;
  uint64_t size_ = 0;
};

// Local-filesystem append-only FileWriter.
class LocalFileWriter : public FileWriter {
 public:
  LocalFileWriter() = default;
  ~LocalFileWriter() override;

  LocalFileWriter(const LocalFileWriter&) = delete;
  LocalFileWriter& operator=(const LocalFileWriter&) = delete;

  Status open(const std::string& path);
  Status append(Slice data) override;
  Status finalize() override;
  uint64_t bytes_written() const override { return bytes_written_; }

 private:
  int fd_ = -1;
  uint64_t bytes_written_ = 0;
};

}  // namespace snii::io
