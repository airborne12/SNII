#include <gtest/gtest.h>

#include "snii/common/status.h"

using snii::Status;
using snii::StatusCode;

TEST(Status, OkIsOk) {
  Status s = Status::OK();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
}

TEST(Status, CorruptionCarriesMessage) {
  Status s = Status::Corruption("bad crc");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
  EXPECT_NE(s.to_string().find("bad crc"), std::string::npos);
}

TEST(Status, ReturnIfErrorShortCircuits) {
  auto fn = []() -> Status {
    SNII_RETURN_IF_ERROR(Status::NotFound("x"));
    return Status::OK();
  };
  EXPECT_EQ(fn().code(), StatusCode::kNotFound);
}
