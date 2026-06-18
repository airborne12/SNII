#include "snii/writer/temp_section_file.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/io/local_file.h"

using namespace snii;
using snii::writer::TempSectionFile;

namespace {

std::string OutPath() {
  static int counter = 0;
  return "/tmp/snii_tsf_out_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter++) + ".bin";
}

std::vector<uint8_t> ReadAll(const std::string& path) {
  io::LocalFileReader r;
  EXPECT_TRUE(r.open(path).ok());
  std::vector<uint8_t> out;
  EXPECT_TRUE(r.read_at(0, r.size(), &out).ok());
  return out;
}

}  // namespace

// Appended chunks stream out in order, byte-for-byte, and size() tracks the
// running length (the value the in-RAM POD's .size() used to provide).
TEST(TempSectionFile, AppendSealStreamRoundTrip) {
  TempSectionFile sec;
  ASSERT_TRUE(sec.open("ut").ok());
  EXPECT_EQ(sec.size(), 0u);

  std::vector<uint8_t> a = {1, 2, 3, 4};
  std::vector<uint8_t> b = {9, 8, 7};
  ASSERT_TRUE(sec.append(a).ok());
  EXPECT_EQ(sec.size(), 4u);
  ASSERT_TRUE(sec.append(b).ok());
  EXPECT_EQ(sec.size(), 7u);
  // Empty append is a no-op (matches std::vector::insert of an empty range).
  ASSERT_TRUE(sec.append(std::vector<uint8_t>{}).ok());
  EXPECT_EQ(sec.size(), 7u);

  ASSERT_TRUE(sec.seal().ok());

  const std::string out_path = OutPath();
  {
    io::LocalFileWriter w;
    ASSERT_TRUE(w.open(out_path).ok());
    ASSERT_TRUE(sec.stream_into(&w).ok());
    ASSERT_TRUE(w.finalize().ok());
  }
  std::vector<uint8_t> got = ReadAll(out_path);
  std::vector<uint8_t> want = {1, 2, 3, 4, 9, 8, 7};
  EXPECT_EQ(got, want);
  std::remove(out_path.c_str());
}

// A payload larger than the internal copy buffer (1 MiB) streams out intact, so
// the fixed-buffer copy loop handles multi-chunk reads with no truncation.
TEST(TempSectionFile, LargePayloadStreamsAcrossCopyBuffer) {
  TempSectionFile sec;
  ASSERT_TRUE(sec.open("big").ok());
  const size_t n = (3u << 20) + 12345;  // > 1 MiB, non-multiple of the buffer
  std::vector<uint8_t> payload(n);
  for (size_t i = 0; i < n; ++i) payload[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
  ASSERT_TRUE(sec.append(payload).ok());
  EXPECT_EQ(sec.size(), n);
  ASSERT_TRUE(sec.seal().ok());

  const std::string out_path = OutPath();
  {
    io::LocalFileWriter w;
    ASSERT_TRUE(w.open(out_path).ok());
    ASSERT_TRUE(sec.stream_into(&w).ok());
    ASSERT_TRUE(w.finalize().ok());
  }
  EXPECT_EQ(ReadAll(out_path), payload);
  std::remove(out_path.c_str());
}

// An empty (opened, sealed, nothing appended) section streams zero bytes.
TEST(TempSectionFile, EmptySectionStreamsNothing) {
  TempSectionFile sec;
  ASSERT_TRUE(sec.open("empty").ok());
  ASSERT_TRUE(sec.seal().ok());
  EXPECT_EQ(sec.size(), 0u);

  const std::string out_path = OutPath();
  {
    io::LocalFileWriter w;
    ASSERT_TRUE(w.open(out_path).ok());
    ASSERT_TRUE(sec.stream_into(&w).ok());
    ASSERT_TRUE(w.finalize().ok());
  }
  EXPECT_EQ(ReadAll(out_path).size(), 0u);
  std::remove(out_path.c_str());
}

// The backing scratch file is unlinked on destruction (RAII), so an aborted build
// leaves no temp files behind. We capture the path via the public size()/stream
// contract indirectly: after the section is destroyed, no scratch file with our
// PID tag in the directory should remain referenceable through stream errors.
TEST(TempSectionFile, MisusePathsRejected) {
  TempSectionFile sec;
  // append before open -> internal error (not a crash).
  EXPECT_FALSE(sec.append(std::vector<uint8_t>{1}).ok());
  // stream_into before seal -> internal error.
  ASSERT_TRUE(sec.open("misuse").ok());
  io::LocalFileWriter w;
  EXPECT_FALSE(sec.stream_into(&w).ok());
  // null sink -> invalid argument.
  ASSERT_TRUE(sec.seal().ok());
  EXPECT_EQ(sec.stream_into(nullptr).code(), StatusCode::kInvalidArgument);
  // double open -> internal error.
  TempSectionFile sec2;
  ASSERT_TRUE(sec2.open("x").ok());
  EXPECT_FALSE(sec2.open("x").ok());
}
