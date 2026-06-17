find_package(GTestPrebuilt REQUIRED)

# 测试源清单按任务显式追加。
set(SNII_TEST_SOURCES
  smoke_test.cpp
  common/status_test.cpp
  common/slice_test.cpp
)

add_executable(snii_core_tests ${SNII_TEST_SOURCES})
target_link_libraries(snii_core_tests PRIVATE snii snii_gtest)

include(GoogleTest)
gtest_discover_tests(snii_core_tests)
