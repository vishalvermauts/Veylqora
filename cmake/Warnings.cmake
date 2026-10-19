# Warning policy for first-party targets only. Third-party headers are consumed
# through SYSTEM includes so their diagnostics never gate our build (ADR-0004).

add_library(veylqora_warnings INTERFACE)

if(MSVC)
  target_compile_options(veylqora_warnings INTERFACE /W4 /permissive- /utf-8)
  if(VEYLQORA_WERROR)
    target_compile_options(veylqora_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(veylqora_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
  if(VEYLQORA_WERROR)
    target_compile_options(veylqora_warnings INTERFACE -Werror)
  endif()
endif()
