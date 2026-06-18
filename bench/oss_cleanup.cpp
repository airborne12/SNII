#include "oss_cleanup.h"

#ifdef SNII_WITH_S3

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/DeleteObjectRequest.h>

#include <memory>
#include <string>
#include <vector>

namespace bench {

namespace {

// Mirrors core/src/io/s3_object_store.cpp make_client(): virtual-hosted addressing
// with payload signing disabled, as required by Aliyun OSS.
std::shared_ptr<Aws::S3::S3Client> make_client(const snii::io::S3Config& cfg) {
  Aws::Auth::AWSCredentials creds(Aws::String(cfg.ak.c_str()),
                                  Aws::String(cfg.sk.c_str()));
  Aws::Client::ClientConfiguration client_cfg;
  client_cfg.endpointOverride = Aws::String(cfg.endpoint.c_str());
  client_cfg.region = Aws::String(cfg.region.c_str());
  return std::make_shared<Aws::S3::S3Client>(
      creds, client_cfg,
      Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
      /*useVirtualAddressing=*/true);
}

}  // namespace

size_t oss_delete_objects(const snii::io::S3Config& cfg,
                          const std::vector<std::string>& full_keys) noexcept {
  size_t deleted = 0;
  try {
    auto client = make_client(cfg);
    for (const std::string& key : full_keys) {
      Aws::S3::Model::DeleteObjectRequest req;
      req.SetBucket(Aws::String(cfg.bucket.c_str()));
      req.SetKey(Aws::String(key.c_str()));
      auto outcome = client->DeleteObject(req);
      if (outcome.IsSuccess()) ++deleted;
    }
  } catch (...) {
    // Cleanup is strictly best-effort.
  }
  return deleted;
}

}  // namespace bench

#endif  // SNII_WITH_S3
