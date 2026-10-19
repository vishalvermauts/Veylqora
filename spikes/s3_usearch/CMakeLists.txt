cmake_minimum_required(VERSION 3.28)
project(spike_s3_usearch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pinned USearch release. Re-pin deliberately; results are only comparable
# within one pin.
include(FetchContent)
FetchContent_Declare(usearch
  GIT_REPOSITORY https://github.com/unum-cloud/usearch.git
  GIT_TAG        v2.26.5
  GIT_SHALLOW    TRUE)
set(USEARCH_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(USEARCH_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(USEARCH_BUILD_LIB_C OFF CACHE BOOL "" FORCE)
set(USEARCH_BUILD_LIB_CPP OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(usearch)

add_executable(spike_s3 main.cpp)
target_link_libraries(spike_s3 PRIVATE usearch)
target_compile_options(spike_s3 PRIVATE -O2)
