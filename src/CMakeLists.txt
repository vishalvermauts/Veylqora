add_library(veylqora_core
  core/identity.cpp)

target_include_directories(veylqora_core
  PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(veylqora_core
  PRIVATE picosha2 veylqora_warnings veylqora_sanitizers)

add_library(veylqora::core ALIAS veylqora_core)
