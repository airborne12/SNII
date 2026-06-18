// S3/OSS round-trip test. Compiled only when the S3 backend is enabled, and
// skipped at runtime unless SNII_OSS_AK / SNII_OSS_SK env vars are present (so
// CI links the code without needing real credentials, and never hardcodes any).
#ifdef SNII_WITH_S3

#include "snii/io/s3_object_store.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

snii::io::S3Config make_config(const char* ak, const char* sk) {
  snii::io::S3Config cfg;
  cfg.endpoint = "oss-cn-hongkong.aliyuncs.com";
  cfg.region = "cn-hongkong";
  cfg.bucket = "doris-community-test";
  cfg.prefix = "cloud_regression/snii_s3_test";
  cfg.ak = ak;
  cfg.sk = sk;
  return cfg;
}

TEST(S3ObjectStoreTest, RoundTrip) {
  const char* ak = std::getenv("SNII_OSS_AK");
  const char* sk = std::getenv("SNII_OSS_SK");
  if (ak == nullptr || sk == nullptr || ak[0] == '\0' || sk[0] == '\0') {
    GTEST_SKIP() << "SNII_OSS_AK / SNII_OSS_SK not set; skipping live OSS test";
  }

  // InitAPI/ShutdownAPI lifecycle for the duration of this test.
  snii::io::AwsApiGuard api_guard;
  const snii::io::S3Config cfg = make_config(ak, sk);
  const std::string key = "roundtrip.bin";

  // Deterministic payload.
  std::vector<uint8_t> payload(1024);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFF);
  }

  // Write.
  snii::io::S3FileWriter writer;
  ASSERT_TRUE(writer.open(cfg, key).ok());
  ASSERT_TRUE(writer.append(snii::Slice(payload)).ok());
  EXPECT_EQ(writer.bytes_written(), payload.size());
  ASSERT_TRUE(writer.finalize().ok());

  // Open reader and check cached size.
  snii::io::S3FileReader reader;
  ASSERT_TRUE(snii::io::S3FileReader::open(cfg, key, &reader).ok());
  EXPECT_EQ(reader.size(), payload.size());

  // Full read.
  std::vector<uint8_t> full;
  ASSERT_TRUE(reader.read_at(0, payload.size(), &full).ok());
  EXPECT_EQ(full, payload);

  // Mid-range read.
  std::vector<uint8_t> mid;
  ASSERT_TRUE(reader.read_at(100, 200, &mid).ok());
  ASSERT_EQ(mid.size(), 200u);
  for (size_t i = 0; i < mid.size(); ++i) {
    EXPECT_EQ(mid[i], payload[100 + i]);
  }

  // Tail read.
  std::vector<uint8_t> tail;
  ASSERT_TRUE(reader.read_at(payload.size() - 16, 16, &tail).ok());
  ASSERT_EQ(tail.size(), 16u);
  for (size_t i = 0; i < tail.size(); ++i) {
    EXPECT_EQ(tail[i], payload[payload.size() - 16 + i]);
  }

  // Reading past EOF is an error.
  std::vector<uint8_t> oob;
  EXPECT_FALSE(reader.read_at(payload.size(), 1, &oob).ok());
}

}  // namespace

#endif  // SNII_WITH_S3
