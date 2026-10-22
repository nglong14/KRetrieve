cmake_minimum_required(VERSION 3.16)
project(KRetrieve LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(KRETRIEVE_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(KRETRIEVE_TSAN "Build with ThreadSanitizer" OFF)

add_library(kretrieve_core
  src/corpus.cpp
  src/index.cpp
  src/prepare.cpp
  src/protocol.cpp
  src/tokenizer.cpp)
target_include_directories(kretrieve_core PUBLIC src)
target_compile_options(kretrieve_core PUBLIC -Wall -Wextra -Wpedantic)

if(KRETRIEVE_ASAN AND KRETRIEVE_TSAN)
  message(FATAL_ERROR "KRETRIEVE_ASAN and KRETRIEVE_TSAN are mutually exclusive")
elseif(KRETRIEVE_ASAN)
  target_compile_options(kretrieve_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(kretrieve_core PUBLIC -fsanitize=address,undefined)
elseif(KRETRIEVE_TSAN)
  target_compile_options(kretrieve_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(kretrieve_core PUBLIC -fsanitize=thread)
endif()

add_executable(kretrieve src/main.cpp)
target_link_libraries(kretrieve PRIVATE kretrieve_core)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)

  add_executable(kretrieve_tests
    tests/corpus_test.cpp
    tests/index_test.cpp
    tests/prepare_test.cpp
    tests/protocol_test.cpp
    tests/tokenizer_test.cpp)
  target_link_libraries(kretrieve_tests PRIVATE kretrieve_core GTest::gtest_main)
  target_compile_definitions(kretrieve_tests PRIVATE
    KRETRIEVE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")

  include(GoogleTest)
  gtest_discover_tests(kretrieve_tests)
  add_test(NAME PrepareCli
    COMMAND ${CMAKE_COMMAND}
      "-DKRETRIEVE=$<TARGET_FILE:kretrieve>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/prepare_cli_test.cmake")
endif()
