#pragma once

// OSS-backed, read-only CLucene Directory for the real-OSS benchmark.
//
// ISOLATION: the entire body of this header (and its .cpp) is guarded by
// SNII_WITH_S3. When the option is OFF the translation unit compiles to nothing
// and pulls in no aws-sdk headers, so the default bench build (no -DSNII_WITH_S3)
// is unaffected. Only when CMake is configured with -DSNII_WITH_S3=ON is the macro
// defined and aws linked.
//
// The directory serves a pre-uploaded CLucene index whose individual segment
// files live under one OSS key prefix. openInput(name) opens a
// snii::io::S3FileReader for prefix + "/" + name, wraps it in a
// snii::io::MeteredFileReader, and returns an IndexInput whose physical reads are
// REAL ranged OSS GETs accounted by the exact same cost model used for the SNII
// reader. Aggregate IoMetrics across all opened inputs, summing + resetting
// between queries, exactly like the local MeteredDirectory in clucene_adapter.cpp.
#ifdef SNII_WITH_S3

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "snii/io/metered_file_reader.h"
#include "snii/io/s3_object_store.h"

// CLucene's store::Directory lives in lucene::store; we keep the concrete CLucene
// types out of this header (it is included by main.cpp) by pimpl'ing them away.
namespace lucene::store {
class Directory;
}  // namespace lucene::store

namespace bench {

// Owns an OSS-backed read-only CLucene Directory plus the metered readers it has
// opened, so their I/O metrics can be aggregated and reset between queries. The
// concrete lucene::store::Directory subclass lives in the .cpp (it needs the full
// CLucene headers); this wrapper exposes only the handle main.cpp needs.
class OssCluceneDirectory {
 public:
  // `files` is the list of segment file names that were uploaded to OSS under
  // cfg.prefix (each as prefix + "/" + name). The directory reports exactly these
  // via list()/fileExists() and opens each via openInput().
  OssCluceneDirectory(const snii::io::S3Config& cfg,
                      std::vector<std::string> files);
  ~OssCluceneDirectory();

  OssCluceneDirectory(const OssCluceneDirectory&) = delete;
  OssCluceneDirectory& operator=(const OssCluceneDirectory&) = delete;

  // The underlying lucene::store::Directory* (non-owning view) to hand to
  // IndexReader::open. Lifetime is tied to this object.
  lucene::store::Directory* directory();

  // Sum of every opened metered reader's metrics.
  snii::io::IoMetrics aggregate_metrics() const;

  // Resets every retained metered reader's counters and cache (cold-cache model)
  // without freeing the readers (live IndexInputs hold raw pointers into them).
  void reset_metrics();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bench

#endif  // SNII_WITH_S3
