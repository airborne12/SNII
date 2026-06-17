find_package(GTestPrebuilt REQUIRED)

# Test sources are picked up via GLOB_RECURSE + CONFIGURE_DEPENDS: a new *_test.cpp is
# discovered automatically, with no edits to this list (so parallel module test
# development does not conflict).
file(GLOB_RECURSE SNII_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)

add_executable(snii_core_tests ${SNII_TEST_SOURCES})
target_link_libraries(snii_core_tests PRIVATE snii snii_gtest)

include(GoogleTest)
gtest_discover_tests(snii_core_tests)
