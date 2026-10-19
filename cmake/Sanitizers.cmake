# Sanitizers are a Clang/GCC-on-Linux capability in this project (ADR-0001):
# MSVC ships AddressSanitizer but has no UndefinedBehaviorSanitizer, so the
# sanitizer gate in Module 3.3 runs only on the Linux reference platform.

add_library(veylqora_sanitizers INTERFACE)

if(VEYLQORA_SANITIZER)
  if(MSVC)
    message(FATAL_ERROR
      "VEYLQORA_SANITIZER is not supported on MSVC. Run sanitizer builds on the "
      "Linux reference platform (see docs/adr/0001-platforms.md).")
  endif()

  set(_san "")
  if(VEYLQORA_SANITIZER MATCHES "address")
    list(APPEND _san address)
  endif()
  if(VEYLQORA_SANITIZER MATCHES "undefined")
    list(APPEND _san undefined)
  endif()
  if(NOT _san)
    message(FATAL_ERROR "Unknown VEYLQORA_SANITIZER value: ${VEYLQORA_SANITIZER}")
  endif()

  list(JOIN _san "," _san_arg)
  target_compile_options(veylqora_sanitizers INTERFACE
    -fsanitize=${_san_arg} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(veylqora_sanitizers INTERFACE -fsanitize=${_san_arg})
  message(STATUS "Sanitizers enabled: ${_san_arg}")
endif()
