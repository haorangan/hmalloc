cmake_minimum_required(VERSION 3.20)
project(hmalloc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build; this is a performance project.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Emit compile_commands.json for editor/LSP tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(hmalloc STATIC
  src/hmalloc.cpp
)
target_include_directories(hmalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hmalloc PUBLIC cxx_std_20)
target_compile_options(hmalloc PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

add_executable(test_basic tests/test_basic.cpp)
target_link_libraries(test_basic PRIVATE hmalloc)
target_compile_options(test_basic PRIVATE -Wall -Wextra)
add_test(NAME test_basic COMMAND test_basic)

add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE hmalloc)
target_compile_options(bench PRIVATE -Wall -Wextra)
