#include "snii/io/local_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace snii::io {
namespace {

std::string errno_msg(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

LocalFileReader::~LocalFileReader() {
  if (fd_ >= 0) ::close(fd_);
}

Status LocalFileReader::open(const std::string& path) {
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ < 0) return Status::IoError(errno_msg("open"));
  struct stat st;
  if (::fstat(fd_, &st) != 0) return Status::IoError(errno_msg("fstat"));
  size_ = static_cast<uint64_t>(st.st_size);
  return Status::OK();
}

Status LocalFileReader::read_at(uint64_t offset, size_t len, std::vector<uint8_t>* out) {
  if (fd_ < 0) return Status::IoError("read_at on unopened file");
  // Non-wrapping bounds check (offset+len could overflow uint64 on a corrupt arg).
  if (offset > size_ || len > size_ - offset) {
    return Status::Corruption("read_at past end of file");
  }
  out->resize(len);
  size_t done = 0;
  while (done < len) {
    ssize_t n = ::pread(fd_, out->data() + done, len - done,
                        static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::IoError(errno_msg("pread"));
    }
    if (n == 0) return Status::Corruption("pread returned 0 before len");
    done += static_cast<size_t>(n);
  }
  return Status::OK();
}

LocalFileWriter::~LocalFileWriter() {
  if (fd_ >= 0) ::close(fd_);
}

Status LocalFileWriter::open(const std::string& path) {
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ < 0) return Status::IoError(errno_msg("open"));
  return Status::OK();
}

Status LocalFileWriter::append(Slice data) {
  if (fd_ < 0) return Status::IoError("append on unopened file");
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = ::write(fd_, data.data() + done, data.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::IoError(errno_msg("write"));
    }
    done += static_cast<size_t>(n);
  }
  bytes_written_ += data.size();
  return Status::OK();
}

Status LocalFileWriter::finalize() {
  if (fd_ < 0) return Status::IoError("finalize on unopened file");
  if (::fsync(fd_) != 0) return Status::IoError(errno_msg("fsync"));
  if (::close(fd_) != 0) {
    fd_ = -1;
    return Status::IoError(errno_msg("close"));
  }
  fd_ = -1;
  return Status::OK();
}

}  // namespace snii::io
