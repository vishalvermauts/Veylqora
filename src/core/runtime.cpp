#include "veylqora/core/runtime.hpp"

#include <format>
#include <random>

namespace veylqora {

std::string RandomIds::next() {
  thread_local std::mt19937_64 rng{[] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd()};
    return std::mt19937_64(seq);
  }()};
  std::string out(32, '0');
  constexpr char kHex[] = "0123456789abcdef";
  const std::uint64_t hi = rng();
  const std::uint64_t lo = rng();
  for (std::size_t i = 0; i < 16; ++i) {
    out[i] = kHex[(hi >> (i * 4)) & 0xF];
    out[16 + i] = kHex[(lo >> (i * 4)) & 0xF];
  }
  return out;
}

std::string to_iso8601(Timestamp t) {
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                     std::chrono::floor<std::chrono::seconds>(t));
}

}  // namespace veylqora
