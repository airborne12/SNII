# Examples: small, self-contained programs that drive the public SNII API.
# Built only when SNII_BUILD_EXAMPLES=ON. Each links libsnii (which propagates its
# zstd/roaring/xxhash deps) + Threads (zstd's pool uses pthreads).

find_package(Threads REQUIRED)

add_executable(doris_writer_demo doris_writer_demo.cpp)
target_link_libraries(doris_writer_demo PRIVATE snii Threads::Threads)
