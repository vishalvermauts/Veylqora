cmake_minimum_required(VERSION 3.28)

project(veylqora
  VERSION 0.0.1
  DESCRIPTION "Code intelligence and persistent agent memory"
  LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(VEYLQORA_BUILD_TESTS "Build the test suite" ON)
option(VEYLQORA_WERROR "Treat first-party warnings as errors" ON)
set(VEYLQORA_SANITIZER "" CACHE STRING "Sanitizer to enable: address, undefined, address+undefined")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Warnings)
include(Sanitizers)
include(Dependencies)

add_subdirectory(src)

if(VEYLQORA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
