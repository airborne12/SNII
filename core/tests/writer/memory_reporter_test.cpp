#include "snii/writer/memory_reporter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace snii::writer {
namespace {

TEST(MemoryReporter, StartsAtZero) {
  MemoryReporter reporter;  // null callback
  EXPECT_EQ(reporter.current_bytes(), 0);
}

TEST(MemoryReporter, TracksPositiveAndNegativeDeltas) {
  MemoryReporter reporter;
  reporter.report(+100);
  reporter.report(+50);
  EXPECT_EQ(reporter.current_bytes(), 150);
  reporter.report(-30);
  EXPECT_EQ(reporter.current_bytes(), 120);
  reporter.report(-120);
  EXPECT_EQ(reporter.current_bytes(), 0);
}

TEST(MemoryReporter, NullCallbackIsSafe) {
  MemoryReporter reporter(nullptr);  // explicit null ConsumeReleaseFn
  reporter.report(+10);
  reporter.report(-10);
  EXPECT_EQ(reporter.current_bytes(), 0);
}

TEST(MemoryReporter, CallbackFiresWithSameDelta) {
  std::vector<int64_t> sink;
  MemoryReporter reporter([&sink](int64_t delta) { sink.push_back(delta); });
  reporter.report(+100);
  reporter.report(-40);
  ASSERT_EQ(sink.size(), 2u);
  EXPECT_EQ(sink[0], 100);
  EXPECT_EQ(sink[1], -40);
  EXPECT_EQ(reporter.current_bytes(), 60);
}

TEST(MemoryReporter, CallbackSumMirrorsCurrentBytes) {
  int64_t external_total = 0;
  MemoryReporter reporter([&external_total](int64_t delta) { external_total += delta; });
  reporter.report(+100);
  reporter.report(+250);
  reporter.report(-75);
  reporter.report(+12);
  reporter.report(-200);
  EXPECT_EQ(external_total, reporter.current_bytes());
}

TEST(MemoryReporter, ZeroDeltaIsNoOpButStillReports) {
  int fire_count = 0;
  MemoryReporter reporter([&fire_count](int64_t) { ++fire_count; });
  reporter.report(+100);
  EXPECT_EQ(reporter.current_bytes(), 100);
  reporter.report(0);
  EXPECT_EQ(reporter.current_bytes(), 100);
  EXPECT_EQ(fire_count, 2);  // report(0) still fires the callback once
}

}  // namespace
}  // namespace snii::writer
