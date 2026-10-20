add_library(veylqora_core
  core/identity.cpp
  core/log.cpp
  core/runtime.cpp
  index/indexer.cpp
  ingestion/process.cpp
  ingestion/source.cpp
  index/python_parser.cpp
  index/javascript_parser.cpp
  graph/resolver.cpp
  graph/query.cpp
  core/project.cpp
  core/file_lock.cpp
  server/server.cpp
  api/service.cpp
  memory/memory.cpp
  retrieval/text.cpp
  retrieval/search.cpp
  memory/transfer.cpp
  observability/observability.cpp
  storage/database.cpp)

target_include_directories(veylqora_core
  PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(veylqora_core
  PUBLIC SQLite::SQLite3
  PRIVATE picosha2 tree_sitter_python tree_sitter_javascript tree_sitter_typescript veylqora_warnings veylqora_sanitizers)

target_compile_definitions(veylqora_core PRIVATE VEYLQORA_VERSION="${PROJECT_VERSION}")
find_package(Threads REQUIRED)
target_link_libraries(veylqora_core PUBLIC nlohmann_json::nlohmann_json httplib::httplib Threads::Threads)

add_library(veylqora::core ALIAS veylqora_core)

add_executable(veylqora cli/main.cpp)
target_compile_definitions(veylqora PRIVATE VEYLQORA_VERSION="${PROJECT_VERSION}")
target_link_libraries(veylqora
  PRIVATE veylqora::core veylqora_warnings veylqora_sanitizers)
