#include "veylqora/core/runtime.hpp"

#include <format>

namespace veylqora {

std::string to_iso8601(Timestamp t) {
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(t));
}

}  // namespace veylqora
