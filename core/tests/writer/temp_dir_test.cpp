#include "snii/writer/temp_dir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace snii::writer {
namespace {

// Saves an env var on construction and restores it on destruction, so these tests
// never leak SNII_TEMP_DIR / TMPDIR into the rest of the suite (which would redirect
// other tests' spill/section temp files to a possibly non-existent directory).
struct EnvGuard {
  std::string name;
  bool had = false;
  std::string old;
  explicit EnvGuard(const char* n) : name(n) {
    const char* v = std::getenv(n);
    had = (v != nullptr);
    if (had) old = v;
  }
  ~EnvGuard() {
    if (had) {
      ::setenv(name.c_str(), old.c_str(), 1);
    } else {
      ::unsetenv(name.c_str());
    }
  }
};

TEST(TempDir, SniiTempDirTakesPrecedenceOverTmpdir) {
  EnvGuard g1("SNII_TEMP_DIR");
  EnvGuard g2("TMPDIR");
  ::setenv("SNII_TEMP_DIR", "/mnt/nvme/scratch", 1);
  ::setenv("TMPDIR", "/var/tmp", 1);
  EXPECT_EQ(resolve_temp_dir(), "/mnt/nvme/scratch");
}

TEST(TempDir, FallsBackTmpdirThenTmp) {
  EnvGuard g1("SNII_TEMP_DIR");
  EnvGuard g2("TMPDIR");
  ::unsetenv("SNII_TEMP_DIR");
  ::setenv("TMPDIR", "/var/tmp/", 1);  // trailing slash stripped
  EXPECT_EQ(resolve_temp_dir(), "/var/tmp");
  ::unsetenv("TMPDIR");
  EXPECT_EQ(resolve_temp_dir(), "/tmp");
}

TEST(TempDir, EmptyEnvIsIgnored) {
  EnvGuard g1("SNII_TEMP_DIR");
  EnvGuard g2("TMPDIR");
  ::setenv("SNII_TEMP_DIR", "", 1);  // empty -> ignored, falls through
  ::unsetenv("TMPDIR");
  EXPECT_EQ(resolve_temp_dir(), "/tmp");
}

TEST(TempDir, AvailableBytesStatsRealDirAndSentinelsOnFailure) {
  EXPECT_NE(temp_dir_available_bytes("/tmp"), UINT64_MAX);  // real dir -> some free
  EXPECT_EQ(temp_dir_available_bytes("/snii_no_such_dir_xyzzy"), UINT64_MAX);
}

}  // namespace
}  // namespace snii::writer
