# M3.3 fuzz targets (libFuzzer). Built only with -DVEYLQORA_FUZZ=ON and Clang,
# which is how the sanitizer gate runs on the Linux reference platform (ADR-0001).

set(VEYLQORA_FUZZ_TARGETS
  fuzz_python_parser
  fuzz_javascript_parser
  fuzz_retrieval_query
  fuzz_memory_json)

foreach(name IN LISTS VEYLQORA_FUZZ_TARGETS)
  add_executable(${name} ${name}.cpp)
  target_compile_definitions(${name} PRIVATE
    "VEYLQORA_FIXTURE_DIR=\"${CMAKE_SOURCE_DIR}/tests/fixtures\"")
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(${name} PRIVATE veylqora::core veylqora_warnings veylqora_sanitizers)
endforeach()
