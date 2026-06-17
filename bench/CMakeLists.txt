# SNII benchmark: compares S3-access metrics of the SNII reader vs the Doris
# CLucene reader on the same corpus. Depends on libsnii (public API) + CLucene.
# This tree is fully separate from core/ (core has zero bench/clucene deps).
find_package(CLucene REQUIRED)

# Bench sources are globbed so new components drop in without editing this file.
file(GLOB_RECURSE SNII_BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(snii_bench ${SNII_BENCH_SOURCES})
target_include_directories(snii_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(snii_bench PRIVATE snii snii_clucene)
