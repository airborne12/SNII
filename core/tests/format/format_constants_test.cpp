#include <gtest/gtest.h>

#include "snii/format/format_constants.h"

using namespace snii::format;

// 锁定 on-disk 契约值，防止意外改动破坏已写文件的可读性。
TEST(FormatConstants, MagicAndVersionStable) {
  EXPECT_EQ(kContainerMagic, 0x49494E53u);
  EXPECT_EQ(kTailMagic, 0x4C494154u);
  EXPECT_EQ(kFormatVersion, 1);
}

TEST(FormatConstants, ConfigToTierMapping) {
  EXPECT_EQ(tier_of(IndexConfig::kDocsOnly), IndexTier::kT1);
  EXPECT_EQ(tier_of(IndexConfig::kDocsPositions), IndexTier::kT2);
  EXPECT_EQ(tier_of(IndexConfig::kDocsPositionsScoring), IndexTier::kT3);
}

TEST(FormatConstants, CapabilityPredicates) {
  EXPECT_FALSE(has_positions(IndexConfig::kDocsOnly));
  EXPECT_TRUE(has_positions(IndexConfig::kDocsPositions));
  EXPECT_TRUE(has_scoring(IndexConfig::kDocsPositionsScoring));
  EXPECT_FALSE(has_scoring(IndexConfig::kDocsPositions));
}

TEST(FormatConstants, DictFlagBitsDistinct) {
  EXPECT_EQ(dict_flags::kKind, 0x01);
  EXPECT_EQ(dict_flags::kEnc, 0x02);
  EXPECT_EQ(dict_flags::kHasSb, 0x04);
}
