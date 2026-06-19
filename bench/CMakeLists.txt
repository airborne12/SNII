# SNII benchmark: compares S3-access metrics of the SNII reader vs the Doris
# CLucene reader on the same corpus. Depends on libsnii (public API) + CLucene.
# This tree is fully separate from core/ (core has zero bench/clucene deps).
find_package(CLucene REQUIRED)

# Bench sources are globbed so new components drop in without editing this file.
# *_test.cpp files are gtest unit tests for individual bench components: they are
# excluded from snii_bench (which links CLucene + carries its own main) and built
# into their own ctest-discovered executables below instead.
file(GLOB_RECURSE SNII_BENCH_ALL CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
set(SNII_BENCH_SOURCES ${SNII_BENCH_ALL})
list(FILTER SNII_BENCH_SOURCES EXCLUDE REGEX "_test\\.cpp$")

add_executable(snii_bench ${SNII_BENCH_SOURCES})
target_include_directories(snii_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(snii_bench PRIVATE snii snii_clucene)

# Real-OSS mode (optional). When SNII_WITH_S3 is ON the bench also compiles the
# OSS-backed CLucene directory + adapters (all guarded by SNII_WITH_S3) and needs
# the aws static archives + the macro to enable that code. libsnii already links
# aws under the same option; the bench links it directly because it #includes the
# aws-sdk headers in oss_cleanup.cpp (DeleteObject, not in the core snii::io API).
if(SNII_WITH_S3)
  find_package(AwsSdk REQUIRED)
  target_compile_definitions(snii_bench PRIVATE SNII_WITH_S3)
  target_link_libraries(snii_bench PRIVATE snii_aws)
endif()

# Bench unit tests (gtest). These deliberately do NOT link CLucene/AWS: the
# corpus_loader test exercises only the pure file-parsing path, so it links just
# corpus_loader.cpp + corpus_gen.cpp (for the Corpus type) + prebuilt gtest.
if(SNII_BUILD_TESTS)
  find_package(GTestPrebuilt REQUIRED)
  enable_testing()
  add_executable(snii_bench_corpus_loader_test
    ${CMAKE_CURRENT_SOURCE_DIR}/corpus_loader_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/corpus_loader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/corpus_gen.cpp)
  target_include_directories(snii_bench_corpus_loader_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(snii_bench_corpus_loader_test PRIVATE snii_gtest)

  include(GoogleTest)
  gtest_discover_tests(snii_bench_corpus_loader_test)
endif()
