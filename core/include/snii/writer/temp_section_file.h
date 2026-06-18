#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/io/file_writer.h"
#include "snii/io/local_file.h"

// TempSectionFile -- an append-only on-disk scratch buffer for ONE index section
// (.frq POD, .prx POD, or DICT region) that the writer would otherwise hold fully
// in RAM until finish().
//
// During the build pass, each term's already-small per-term bytes are appended
// here instead of into a std::vector<uint8_t>; only a running byte cursor is kept
// in memory (size() == frq_pod_.size() used to). At finish(), the orchestrator
// streams the temp file's bytes into the container via stream_into(), using a
// fixed copy buffer (no whole-section reload). Because the append order and byte
// content are unchanged, the produced container is BYTE-IDENTICAL to the in-RAM
// path; only the peak working set drops (the section lives on disk, not in RAM).
//
// RAII: the backing file is unlinked on destruction on every path (success or
// exception), so an aborted build leaves no scratch files behind.
namespace snii::writer {

class TempSectionFile {
 public:
  TempSectionFile() = default;
  ~TempSectionFile();

  TempSectionFile(const TempSectionFile&) = delete;
  TempSectionFile& operator=(const TempSectionFile&) = delete;

  // Opens a fresh, process-unique scratch file under the temp dir. `tag` is a
  // short ASCII discriminator (e.g. "frq") folded into the path for debugging.
  // Idempotent-unsafe: call once per instance.
  Status open(const char* tag);

  // Appends bytes to the scratch file, advancing the cursor. A no-op for empty
  // input (matching std::vector::insert of an empty range).
  Status append(const std::vector<uint8_t>& bytes);
  Status append(Slice bytes);

  // Running byte length written so far (the value the in-RAM POD's .size() had).
  uint64_t size() const { return size_; }

  // Flushes + closes the write side so the file can be re-read. Must be called
  // once after the last append and before stream_into().
  Status seal();

  // Streams the entire scratch file into `out` (append-only) using a fixed
  // copy buffer. Requires seal() to have been called. Safe for an empty file.
  Status stream_into(snii::io::FileWriter* out) const;

 private:
  std::string path_;
  snii::io::LocalFileWriter writer_;
  uint64_t size_ = 0;
  bool sealed_ = false;
  bool open_ = false;
};

}  // namespace snii::writer
