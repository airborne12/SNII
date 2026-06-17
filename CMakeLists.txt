cmake_minimum_required(VERSION 3.29)
project(snii LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror=return-type)

option(SNII_BUILD_TESTS "Build core unit tests" ON)
option(SNII_BUILD_BENCH "Build benchmark (depends on clucene)" OFF)
option(SNII_WITH_S3 "Build S3 backend (depends on aws-sdk-cpp)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(SNII_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)

if(SNII_BUILD_BENCH)
  add_subdirectory(bench)
endif()
