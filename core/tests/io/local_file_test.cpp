#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "snii/common/status.h"
#include "snii/io/local_file.h"

using namespace snii;
using snii::io::LocalFileReader;
using snii::io::LocalFileWriter;

namespace {

std::string TempPath(const char* name) {
  return std::string("/tmp/snii_io_test_") + name + ".bin";
}

}  // namespace

TEST(LocalFile, AppendThenReadBack) {
  const std::string path = TempPath("append");
  {
    LocalFileWriter w;
    ASSERT_TRUE(w.open(path).ok());
    const uint8_t a[] = {1, 2, 3, 4};
    const uint8_t b[] = {5, 6, 7, 8};
    ASSERT_TRUE(w.append(Slice(a, 4)).ok());
    ASSERT_TRUE(w.append(Slice(b, 4)).ok());
    EXPECT_EQ(w.bytes_written(), 8u);
    ASSERT_TRUE(w.finalize().ok());
  }

  LocalFileReader r;
  ASSERT_TRUE(r.open(path).ok());
  EXPECT_EQ(r.size(), 8u);
  std::vector<uint8_t> out;
  ASSERT_TRUE(r.read_at(2, 4, &out).ok());
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], 3u);
  EXPECT_EQ(out[3], 6u);
}

TEST(LocalFile, ReadPastEndFails) {
  const std::string path = TempPath("past_end");
  {
    LocalFileWriter w;
    ASSERT_TRUE(w.open(path).ok());
    const uint8_t a[] = {1, 2, 3};
    ASSERT_TRUE(w.append(Slice(a, 3)).ok());
    ASSERT_TRUE(w.finalize().ok());
  }
  LocalFileReader r;
  ASSERT_TRUE(r.open(path).ok());
  std::vector<uint8_t> out;
  EXPECT_FALSE(r.read_at(2, 10, &out).ok());
}

TEST(LocalFile, OpenMissingFails) {
  LocalFileReader r;
  EXPECT_FALSE(r.open("/tmp/snii_io_test_does_not_exist_zzz.bin").ok());
}
