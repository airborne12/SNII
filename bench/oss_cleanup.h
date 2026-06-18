#pragma once

// Best-effort OSS object cleanup for the benchmark runner.
//
// ISOLATION: guarded by SNII_WITH_S3 so the default bench build (no aws) is
// unaffected. The core snii::io API exposes no DeleteObject, so this helper uses
// the aws SDK directly (the bench already links aws under SNII_WITH_S3) to remove
// the objects the run uploaded. Failures are swallowed (cleanup is non-fatal).
#ifdef SNII_WITH_S3

#include <string>
#include <vector>

#include "snii/io/s3_object_store.h"

namespace bench {

// Deletes each full object key (e.g. "prefix/name") from cfg.bucket. Returns the
// number of objects successfully deleted. Never throws.
size_t oss_delete_objects(const snii::io::S3Config& cfg,
                          const std::vector<std::string>& full_keys) noexcept;

}  // namespace bench

#endif  // SNII_WITH_S3
