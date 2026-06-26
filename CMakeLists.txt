cmake_minimum_required(VERSION 3.19.2)
project(snii LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RELEASE CACHE STRING "build type" FORCE)
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING "build type" FORCE)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(OS_LINUX 1)
  add_definitions(-D OS_LINUX)
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
  set(OS_MACOSX 1)
  add_definitions(-D OS_MACOSX)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(COMPILER_GCC 1)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(COMPILER_CLANG 1)
endif()

execute_process(COMMAND bash "-c" "uname -m"
                OUTPUT_VARIABLE CMAKE_BUILD_TARGET_ARCH
                OUTPUT_STRIP_TRAILING_WHITESPACE)

add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
add_compile_options(-g
                    -gdwarf-5
                    -Wall
                    -Wextra
                    -Werror
                    -Wundef
                    -pthread
                    -fstrict-aliasing
                    -fno-omit-frame-pointer
                    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>)
add_compile_options(-Wno-unused-parameter
                    -Wno-sign-compare)

if(COMPILER_GCC)
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "11.1")
    message(FATAL_ERROR "Need GCC version at least 11.1")
  endif()
  add_compile_options(-fdiagnostics-color=always
                      -Wno-nonnull
                      -Wno-stringop-overread
                      -Wno-stringop-overflow
                      -Wno-array-bounds
                      -Wno-maybe-uninitialized)
endif()

if(COMPILER_CLANG)
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16")
    message(FATAL_ERROR "Need Clang version at least 16")
  endif()
  add_compile_options(-fcolor-diagnostics
                      -Wpedantic
                      -Wshadow
                      -Wshadow-field
                      -Wunused
                      -Wunused-command-line-argument
                      -Wunused-exception-parameter
                      -Wunused-volatile-lvalue
                      -Wunused-template
                      -Wunused-member-function
                      -Wunused-macros
                      -Wconversion
                      -Wthread-safety)
  add_compile_options(-Wno-gnu-statement-expression
                      -Wno-implicit-float-conversion
                      -Wno-sign-conversion)
endif()

if("${CMAKE_BUILD_TARGET_ARCH}" STREQUAL "x86" OR
   "${CMAKE_BUILD_TARGET_ARCH}" STREQUAL "x86_64")
  add_compile_options(-msse4.2)
endif()

set(CXX_FLAGS_DEBUG "-O0")
set(CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CXX_FLAGS_ASAN "-O0 -fsanitize=address -fsanitize=undefined -fno-sanitize=float-cast-overflow -DUNDEFINED_BEHAVIOR_SANITIZER -DADDRESS_SANITIZER")
set(CXX_FLAGS_LSAN "-O0 -fsanitize=leak -DLEAK_SANITIZER")
set(CXX_FLAGS_ASAN_UT "-O0 -fsanitize=address -DADDRESS_SANITIZER")
set(CXX_FLAGS_UBSAN "-O0 -fno-wrapv -mcmodel=medium -fsanitize=undefined -DUNDEFINED_BEHAVIOR_SANITIZER")
set(CXX_FLAGS_TSAN "-O1 -fsanitize=thread -DTHREAD_SANITIZER -Wno-missing-declarations")

if("${CMAKE_BUILD_TYPE}" STREQUAL "DEBUG")
  set(CMAKE_CXX_FLAGS "${CXX_FLAGS_DEBUG}")
elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
  set(CMAKE_CXX_FLAGS "${CXX_FLAGS_RELEASE}")
elseif("${CMAKE_BUILD_TYPE}" STREQUAL "ASAN")
  set(CMAKE_CXX_FLAGS "${CXX_FLAGS_ASAN}")
  add_definitions(-DNDEBUG_SANITIZER)
elseif("${CMAKE_BUILD_TYPE}" STREQUAL "LSAN")
  set(CMAKE_CXX_FLAGS "${CXX_FLAGS_LSAN}")
elseif("${CMAKE_BUILD_TYPE}" STREQUAL "UBSAN")
  set(CMAKE_CXX_FLAGS "${CXX_FLAGS_UBSAN}")
elseif("${CMAKE_BUILD_TYPE}" STREQUAL "TSAN")
  set(CMAKE_CXX_FLAGS "${CXX_FLAGS_TSAN}")
elseif("${CMAKE_BUILD_TYPE}" STREQUAL "ASAN_UT")
  set(CMAKE_CXX_FLAGS "${CXX_FLAGS_ASAN_UT}")
  add_definitions(-DNDEBUG_SANITIZER)
else()
  message(FATAL_ERROR "Unknown build type: ${CMAKE_BUILD_TYPE}")
endif()
set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS}")

message(STATUS "Build type is ${CMAKE_BUILD_TYPE}")
message(STATUS "Build target arch is ${CMAKE_BUILD_TARGET_ARCH}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "CXX Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "CXX Flags: ${CMAKE_CXX_FLAGS}")

option(SNII_BUILD_TESTS "Build core unit tests" ON)
option(SNII_BUILD_BENCH "Build benchmark (depends on clucene)" OFF)
option(SNII_BUILD_EXAMPLES "Build examples (public-API demos, libsnii only)" OFF)
option(SNII_WITH_S3 "Build S3 backend (depends on aws-sdk-cpp)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(SNII_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)

if(SNII_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(SNII_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
