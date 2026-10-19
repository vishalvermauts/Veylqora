add_executable(veylqora_unit_tests
  unit/test_identity.cpp
  unit/test_python_symbols.cpp
  unit/test_runtime.cpp)

target_link_libraries(veylqora_unit_tests
  PRIVATE veylqora::core Catch2::Catch2WithMain
          veylqora_warnings veylqora_sanitizers)

catch_discover_tests(veylqora_unit_tests)

# The vertical slice: index a fixed 10-file repository, search, restart, and
# check that the answers are identical.
add_executable(veylqora_integration_tests
  integration/test_slice.cpp)

target_compile_definitions(veylqora_integration_tests
  PRIVATE "VEYLQORA_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

target_link_libraries(veylqora_integration_tests
  PRIVATE veylqora::core Catch2::Catch2WithMain
          veylqora_warnings veylqora_sanitizers)

catch_discover_tests(veylqora_integration_tests)
