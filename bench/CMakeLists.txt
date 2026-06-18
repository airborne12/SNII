# SNII benchmark: compares S3-access metrics of the SNII reader vs the Doris
# CLucene reader on the same corpus. Depends on libsnii (public API) + CLucene.
# This tree is fully separate from core/ (core has zero bench/clucene deps).
find_package(CLucene REQUIRED)

# Bench sources are globbed so new components drop in without editing this file.
file(GLOB_RECURSE SNII_BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

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
