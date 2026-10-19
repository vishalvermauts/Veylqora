add_executable(veylqora_unit_tests
  unit/test_identity.cpp)

target_link_libraries(veylqora_unit_tests
  PRIVATE veylqora::core Catch2::Catch2WithMain
          veylqora_warnings veylqora_sanitizers)

catch_discover_tests(veylqora_unit_tests)
