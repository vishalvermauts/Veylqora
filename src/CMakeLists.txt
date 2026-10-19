add_library(veylqora_core
  core/identity.cpp
  core/log.cpp
  core/runtime.cpp
  index/indexer.cpp
  ingestion/process.cpp
  ingestion/source.cpp
  index/python_symbols.cpp
  storage/database.cpp)

target_include_directories(veylqora_core
  PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(veylqora_core
  PUBLIC SQLite::SQLite3
  PRIVATE picosha2 veylqora_warnings veylqora_sanitizers)

add_library(veylqora::core ALIAS veylqora_core)

add_executable(veylqora cli/main.cpp)
target_compile_definitions(veylqora PRIVATE VEYLQORA_VERSION="${PROJECT_VERSION}")
target_link_libraries(veylqora
  PRIVATE veylqora::core veylqora_warnings veylqora_sanitizers)
