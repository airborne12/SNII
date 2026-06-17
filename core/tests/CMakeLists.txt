find_package(GTestPrebuilt REQUIRED)

# 测试源用 GLOB_RECURSE + CONFIGURE_DEPENDS 自动纳入：新增 *_test.cpp 即被发现，
# 无需编辑本清单（并行开发各模块测试时不冲突）。
file(GLOB_RECURSE SNII_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)

add_executable(snii_core_tests ${SNII_TEST_SOURCES})
target_link_libraries(snii_core_tests PRIVATE snii snii_gtest)

include(GoogleTest)
gtest_discover_tests(snii_core_tests)
