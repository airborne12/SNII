#pragma once

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/io/local_file.h"
#include "snii/writer/temp_dir.h"

namespace snii::writer {

// A tiered append buffer for one build-time section: bytes stay in RAM until the
// accumulated size crosses `cap_bytes`, at which point the buffer SPILLS to a temp
// file (under resolve_temp_dir()) and routes all later appends there. So a small
// section costs zero disk and no temp-dir dependency (RAM only), while a large one
// stays RSS-bounded at ~cap_bytes. The append order and bytes are identical wherever
// they land; stream_into() reproduces the whole section in order. RAII-removes the
// temp file. (cap_bytes == UINT64_MAX disables spilling -> always RAM.)
class SpillableByteBuffer {
 public:
  SpillableByteBuffer(uint64_t cap_bytes, std::string tag)
      : cap_bytes_(cap_bytes), tag_(std::move(tag)) {}
  ~SpillableByteBuffer() {
    if (!temp_path_.empty()) std::remove(temp_path_.c_str());
  }
  SpillableByteBuffer(const SpillableByteBuffer&) = delete;
  SpillableByteBuffer& operator=(const SpillableByteBuffer&) = delete;

  // Total bytes appended so far (the offset basis for callers recording sub-offsets).
  uint64_t size() const { return spilled_ ? spilled_bytes_ : ram_.size(); }

  Status append(Slice bytes) {
    if (spilled_) {
      SNII_RETURN_IF_ERROR(temp_.append(bytes));
      spilled_bytes_ += bytes.size();
      return Status::OK();
    }
    ram_.put_bytes(bytes);
    if (ram_.size() >= cap_bytes_) return spill_to_disk();
    return Status::OK();
  }
  Status append(const std::vector<uint8_t>& bytes) { return append(Slice(bytes)); }

  // Must be called once after the last append, before stream_into(): flushes the temp
  // (if spilled) so it can be read back. A no-op for a RAM-resident buffer.
  Status seal() {
    if (spilled_ && !sealed_) {
      SNII_RETURN_IF_ERROR(temp_.finalize());
      sealed_ = true;
    }
    return Status::OK();
  }

  // Streams the whole section (RAM or sealed temp) into `out`, in append order.
  Status stream_into(snii::io::FileWriter* out) const {
    if (!spilled_) return out->append(ram_.view());
    snii::io::LocalFileReader r;
    SNII_RETURN_IF_ERROR(r.open(temp_path_));
    constexpr uint64_t kChunk = 1u << 20;  // fixed copy window (no whole-section reload)
    std::vector<uint8_t> buf;
    for (uint64_t off = 0; off < spilled_bytes_; off += kChunk) {
      const uint64_t n = std::min(kChunk, spilled_bytes_ - off);
      SNII_RETURN_IF_ERROR(r.read_at(off, n, &buf));
      SNII_RETURN_IF_ERROR(out->append(Slice(buf)));
    }
    return Status::OK();
  }

  bool spilled() const { return spilled_; }

 private:
  Status spill_to_disk() {
    temp_path_ = resolve_temp_dir() + "/snii_" + tag_ + "_" +
                 std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".tmp";
    SNII_RETURN_IF_ERROR(temp_.open(temp_path_));
    SNII_RETURN_IF_ERROR(temp_.append(ram_.view()));
    spilled_bytes_ = ram_.size();
    ram_.release();  // hand-off complete: reclaim the RAM immediately
    spilled_ = true;
    return Status::OK();
  }

  uint64_t cap_bytes_;
  std::string tag_;
  ByteSink ram_;
  bool spilled_ = false;
  bool sealed_ = false;
  snii::io::LocalFileWriter temp_;
  std::string temp_path_;
  uint64_t spilled_bytes_ = 0;
};

}  // namespace snii::writer
