#include <gtest/gtest.h>

#include "snii/version.h"

TEST(Smoke, VersionDefined) {
  EXPECT_STREQ(SNII_VERSION_STRING, "0.1.0");
}
