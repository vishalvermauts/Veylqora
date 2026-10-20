#include "veylqora/core/runtime.hpp"

#include <format>
#include <random>

namespace veylqora {

std::string RandomIds::next() {
  std::random_device rd;
  std::string out(32, '0');
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < 32; ++i) out[i] = kHex[rd() & 0xF];
  return out;
}

std::string to_iso8601(Timestamp t) {
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                     std::chrono::floor<std::chrono::seconds>(t));
}

}  // namespace veylqora
