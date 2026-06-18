#include "oss_clucene_directory.h"

#ifdef SNII_WITH_S3

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "CLucene.h"
#include "CLucene/store/Directory.h"
#include "CLucene/store/IndexInput.h"
#include "snii/common/status.h"
#include "snii/io/metered_file_reader.h"
#include "snii/io/s3_object_store.h"

namespace bench {

namespace cl_store = lucene::store;

namespace {

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error("OSS CLucene directory: " + what);
}

}  // namespace

// A BufferedIndexInput backed by a snii MeteredFileReader whose inner reader is a
// real S3FileReader. Identical in shape to the local MeteredIndexInput in
// clucene_adapter.cpp: every buffered readInternal() becomes one
// MeteredFileReader::read_at, i.e. one real ranged OSS GET accounted by the shared
// object-storage cost model. Clones share the SAME MeteredFileReader so all reads
// of one file aggregate into one metric set.
class OssMeteredIndexInput : public cl_store::BufferedIndexInput {
 public:
  OssMeteredIndexInput(snii::io::MeteredFileReader* reader, int64_t length)
      : reader_(reader), length_(length), pos_(0) {}

  OssMeteredIndexInput(const OssMeteredIndexInput& other)
      : cl_store::BufferedIndexInput(other),
        reader_(other.reader_),
        length_(other.length_),
        pos_(other.pos_) {}

  cl_store::IndexInput* clone() const override {
    return _CLNEW OssMeteredIndexInput(*this);
  }

  int64_t length() const override { return length_; }

  const char* getDirectoryType() const override { return "OssCluceneDirectory"; }
  const char* getObjectName() const override { return getClassName(); }
  static const char* getClassName() { return "OssMeteredIndexInput"; }

  void close() override {}

 protected:
  void readInternal(uint8_t* b, const int32_t len) override {
    if (len <= 0) return;
    std::vector<uint8_t> buf;
    const snii::Status s = reader_->read_at(
        static_cast<uint64_t>(pos_), static_cast<size_t>(len), &buf);
    if (!s.ok() || buf.size() != static_cast<size_t>(len)) {
      _CLTHROWA(CL_ERR_IO, "OssMeteredIndexInput: short read");
    }
    std::memcpy(b, buf.data(), buf.size());
    pos_ += len;
  }

  void seekInternal(const int64_t pos) override { pos_ = pos; }

 private:
  snii::io::MeteredFileReader* reader_;  // not owned (lives in the directory)
  int64_t length_;
  int64_t pos_;
};

// A read-only lucene::store::Directory whose openInput() serves files from OSS via
// an S3FileReader wrapped in a MeteredFileReader. Writes/locks are no-ops or throw
// (the index is pre-uploaded and never mutated here). The metered readers are
// retained so their metrics can be aggregated and reset between queries.
class OssDirectory : public cl_store::Directory {
 public:
  OssDirectory(snii::io::S3Config cfg, std::vector<std::string> files)
      : cfg_(std::move(cfg)), files_(std::move(files)) {}

  ~OssDirectory() override = default;

  snii::io::IoMetrics aggregate_metrics() const {
    snii::io::IoMetrics total;
    for (const auto& mr : metered_) {
      const snii::io::IoMetrics& m = mr->metrics();
      total.read_at_calls += m.read_at_calls;
      total.serial_rounds += m.serial_rounds;
      total.range_gets += m.range_gets;
      total.remote_bytes += m.remote_bytes;
      total.total_request_bytes += m.total_request_bytes;
    }
    return total;
  }

  // Resets every retained metered reader's counters AND cache, modelling a cold
  // cache for the next query. The reader objects are NOT freed: live IndexInputs
  // opened by the IndexReader hold raw pointers into them and must keep reading
  // across queries.
  void reset_metrics() {
    for (auto& mr : metered_) mr->reset_metrics();
  }

  bool list(std::vector<std::string>* names) const override {
    names->assign(files_.begin(), files_.end());
    return true;
  }

  bool fileExists(const char* name) const override {
    return std::find(files_.begin(), files_.end(), std::string(name)) !=
           files_.end();
  }

  int64_t fileModified(const char* /*name*/) const override {
    // The uploaded index is immutable; a constant timestamp is sufficient for the
    // read-only search path (CLucene only uses it for segment-staleness checks,
    // which never trigger here).
    return 0;
  }

  int64_t fileLength(const char* name) const override {
    auto it = sizes_.find(name);
    if (it != sizes_.end()) return static_cast<int64_t>(it->second);
    // Resolve lazily via a HeadObject (S3FileReader::open caches size()).
    snii::io::S3FileReader r;
    if (snii::Status s = snii::io::S3FileReader::open(cfg_, name, &r); !s.ok()) {
      return -1;
    }
    const uint64_t sz = r.size();
    sizes_[name] = sz;
    return static_cast<int64_t>(sz);
  }

  void touchFile(const char* /*name*/) override {
    _CLTHROWA(CL_ERR_UnsupportedOperation, "OssDirectory is read-only");
  }
  void renameFile(const char* /*from*/, const char* /*to*/) override {
    _CLTHROWA(CL_ERR_UnsupportedOperation, "OssDirectory is read-only");
  }
  cl_store::IndexOutput* createOutput(const char* /*name*/) override {
    _CLTHROWA(CL_ERR_UnsupportedOperation, "OssDirectory is read-only");
  }
  void close() override { /* directory lifetime managed by the wrapper */ }
  std::string toString() const override { return "OssCluceneDirectory"; }
  const char* getObjectName() const override { return getClassName(); }
  static const char* getClassName() { return "OssCluceneDirectory"; }

  bool openInput(const char* name, cl_store::IndexInput*& ret,
                 CLuceneError& error, int32_t /*bufferSize*/) override {
    auto s3 = std::make_unique<snii::io::S3FileReader>();
    if (snii::Status s = snii::io::S3FileReader::open(cfg_, name, s3.get());
        !s.ok()) {
      error.set(CL_ERR_IO, "OssDirectory: cannot open OSS object");
      return false;
    }
    sizes_[name] = s3->size();
    auto metered = std::make_unique<snii::io::MeteredFileReader>(s3.get());
    const int64_t len = static_cast<int64_t>(metered->size());
    ret = _CLNEW OssMeteredIndexInput(metered.get(), len);
    s3_readers_.push_back(std::move(s3));
    metered_.push_back(std::move(metered));
    return true;
  }

 protected:
  bool doDeleteFile(const char* /*name*/) override {
    _CLTHROWA(CL_ERR_UnsupportedOperation, "OssDirectory is read-only");
  }

 private:
  snii::io::S3Config cfg_;
  std::vector<std::string> files_;
  mutable std::unordered_map<std::string, uint64_t> sizes_;
  std::vector<std::unique_ptr<snii::io::S3FileReader>> s3_readers_;
  std::vector<std::unique_ptr<snii::io::MeteredFileReader>> metered_;
};

struct OssCluceneDirectory::Impl {
  std::unique_ptr<OssDirectory> dir;
};

OssCluceneDirectory::OssCluceneDirectory(const snii::io::S3Config& cfg,
                                         std::vector<std::string> files)
    : impl_(std::make_unique<Impl>()) {
  impl_->dir = std::make_unique<OssDirectory>(cfg, std::move(files));
  if (impl_->dir == nullptr) fail("failed to construct OSS directory");
}

OssCluceneDirectory::~OssCluceneDirectory() = default;

cl_store::Directory* OssCluceneDirectory::directory() {
  return impl_->dir.get();
}

snii::io::IoMetrics OssCluceneDirectory::aggregate_metrics() const {
  return impl_->dir->aggregate_metrics();
}

void OssCluceneDirectory::reset_metrics() { impl_->dir->reset_metrics(); }

}  // namespace bench

#endif  // SNII_WITH_S3
