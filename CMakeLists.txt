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

# Optional sanitizer build: -DHM_SANITIZE=address|thread|undefined (or a
# comma-separated combo address,undefined). Applied to every target. Use thread
# for the lock-free cross-thread paths; address/undefined for everything else.
set(HM_SANITIZE "" CACHE STRING "Sanitizer(s) to build with (address|thread|undefined)")
if(HM_SANITIZE)
  add_compile_options(-fsanitize=${HM_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${HM_SANITIZE})
endif()

# ----------------------------------------------------------------------------
# Library
# ----------------------------------------------------------------------------
add_library(hmalloc STATIC
  src/os.cpp
  src/central.cpp
  src/heap.cpp
  src/hmalloc.cpp
)
target_include_directories(hmalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hmalloc PUBLIC cxx_std_20)
target_compile_options(hmalloc PRIVATE -Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)
target_link_libraries(hmalloc PUBLIC Threads::Threads)

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
enable_testing()

# Some unit tests link the allocator's internal .cpp files directly so they can
# exercise modules below the public API. Keep that list in sync as modules land.
set(HM_INTERNAL_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/os.cpp
)

# add_unit_test(<name> <src> [LINK_INTERNAL])
#   LINK_INTERNAL also compiles the internal sources for white-box tests.
function(add_unit_test name src)
  cmake_parse_arguments(ARG "LINK_INTERNAL" "" "" ${ARGN})
  if(ARG_LINK_INTERNAL)
    add_executable(${name} ${src} ${HM_INTERNAL_SRCS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  else()
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE hmalloc)
  endif()
  target_link_libraries(${name} PRIVATE Threads::Threads)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_unit_test(test_basic       tests/test_basic.cpp)
add_unit_test(test_os          tests/test_os.cpp          LINK_INTERNAL)
add_unit_test(test_size_class  tests/test_size_class.cpp  LINK_INTERNAL)
add_unit_test(test_alloc       tests/test_alloc.cpp)
add_unit_test(test_threads     tests/test_threads.cpp)

# ----------------------------------------------------------------------------
# Benchmarks
# ----------------------------------------------------------------------------
add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE hmalloc Threads::Threads)
target_compile_options(bench PRIVATE -Wall -Wextra)
