add_executable(veylqora_unit_tests
  unit/test_identity.cpp
  unit/test_python_parser.cpp
  unit/test_source.cpp
  unit/test_runtime.cpp)

target_link_libraries(veylqora_unit_tests
  PRIVATE veylqora::core Catch2::Catch2WithMain
          veylqora_warnings veylqora_sanitizers)

catch_discover_tests(veylqora_unit_tests)

# The vertical slice: index a fixed 10-file repository, search, restart, and
# check that the answers are identical.
add_executable(veylqora_integration_tests
  integration/test_api.cpp
  integration/test_graph.cpp
  integration/test_memory.cpp
  integration/test_ingestion.cpp
  integration/test_slice.cpp
  integration/test_staleness.cpp)

target_compile_definitions(veylqora_integration_tests
  PRIVATE "VEYLQORA_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

target_include_directories(veylqora_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(veylqora_integration_tests
  PRIVATE veylqora::core Catch2::Catch2WithMain
          veylqora_warnings veylqora_sanitizers)

catch_discover_tests(veylqora_integration_tests)

# The CLI smoke test runs the real binary as separate processes. It needs bash,
# so it is registered on Unix only; the Windows build is verified by hand.
if(UNIX)
  find_program(BASH_PROGRAM bash REQUIRED)
  add_test(NAME cli_slice
    COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_slice.sh
            $<TARGET_FILE:veylqora> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/python)
endif()
