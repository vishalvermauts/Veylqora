// M3.3 fuzz target: JavaScript and TypeScript source is untrusted input. The
// first byte chooses the dialect; the rest is the source. Only a result or a
// veylqora::Error is acceptable, never a crash.

#include "veylqora/core/error.hpp"
#include "veylqora/index/javascript_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  const auto dialect =
      (data[0] & 1) != 0 ? veylqora::JsDialect::typescript : veylqora::JsDialect::javascript;
  const std::string_view source(reinterpret_cast<const char*>(data + 1), size - 1);
  try {
    (void)veylqora::parse_javascript("app.fuzz", "app", source, dialect);
  } catch (const veylqora::Error&) {
    // Too large to parse is a reported error, not a fault.
  }
  return 0;
}
