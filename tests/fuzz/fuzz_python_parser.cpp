// M3.3 fuzz target: Python source is untrusted input. The only acceptable outcome
// for any byte string is a parsed result or a veylqora::Error, never a crash.

#include "veylqora/core/error.hpp"
#include "veylqora/index/python_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view source(reinterpret_cast<const char*>(data), size);
  try {
    (void)veylqora::parse_python("pkg.fuzz", source, false);
  } catch (const veylqora::Error&) {
    // Too large to parse is a reported error, not a fault.
  }
  return 0;
}
