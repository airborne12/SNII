#include "snii/writer/temp_section_file.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "snii/io/local_file.h"

namespace snii::writer {

namespace {

// Fixed streamed-copy buffer (no whole-section reload). 1 MiB balances syscall
// count against transient RAM during the finish()-time concatenation.
constexpr size_t kCopyBufBytes = 1u << 20;

// Resolve the scratch directory: honor TMPDIR when set, else /tmp. ASCII only.
std::string TempDir() {
  const char* env = std::getenv("TMPDIR");
  if (env != nullptr && env[0] != '\0') {
    std::string d(env);
    if (d.back() == '/') d.pop_back();
    return d;
  }
  return "/tmp";
}

// Process-unique scratch path (pid + monotonic counter so concurrent builds /
// multiple sections never collide).
std::string MakeTempPath(const char* tag) {
  static std::atomic<uint64_t> counter{0};
  const uint64_t n = counter.fetch_add(1);
  return TempDir() + "/snii_sec_" + tag + "_" + std::to_string(::getpid()) + "_" +
         std::to_string(n) + ".tmp";
}

}  // namespace

TempSectionFile::~TempSectionFile() {
  // Unlink on every path (incl. exceptions): writer_ closes its fd in its own
  // dtor; removing the path reclaims the disk bytes for an aborted build too.
  if (!path_.empty()) std::remove(path_.c_str());
}

Status TempSectionFile::open(const char* tag) {
  if (open_) return Status::Internal("temp_section: open called twice");
  path_ = MakeTempPath(tag);
  SNII_RETURN_IF_ERROR(writer_.open(path_));
  open_ = true;
  return Status::OK();
}

Status TempSectionFile::append(Slice bytes) {
  if (!open_) return Status::Internal("temp_section: append before open");
  if (sealed_) return Status::Internal("temp_section: append after seal");
  if (bytes.empty()) return Status::OK();
  SNII_RETURN_IF_ERROR(writer_.append(bytes));
  size_ += bytes.size();
  return Status::OK();
}

Status TempSectionFile::append(const std::vector<uint8_t>& bytes) {
  return append(Slice(bytes));
}

Status TempSectionFile::seal() {
  if (!open_) return Status::Internal("temp_section: seal before open");
  if (sealed_) return Status::OK();
  SNII_RETURN_IF_ERROR(writer_.finalize());
  sealed_ = true;
  return Status::OK();
}

Status TempSectionFile::stream_into(snii::io::FileWriter* out) const {
  if (out == nullptr) return Status::InvalidArgument("temp_section: null sink");
  if (!sealed_) return Status::Internal("temp_section: stream_into before seal");
  if (size_ == 0) return Status::OK();

  snii::io::LocalFileReader reader;
  SNII_RETURN_IF_ERROR(reader.open(path_));
  std::vector<uint8_t> buf;
  uint64_t off = 0;
  while (off < size_) {
    const size_t chunk =
        static_cast<size_t>(std::min<uint64_t>(kCopyBufBytes, size_ - off));
    SNII_RETURN_IF_ERROR(reader.read_at(off, chunk, &buf));
    SNII_RETURN_IF_ERROR(out->append(Slice(buf.data(), chunk)));
    off += chunk;
  }
  return Status::OK();
}

}  // namespace snii::writer
