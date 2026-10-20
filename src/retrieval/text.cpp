#include "veylqora/retrieval/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace veylqora {
namespace {

bool is_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

// FNV-1a: a fixed hash, so vectors are the same on every platform and run.
std::uint64_t fnv1a(std::string_view s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

}  // namespace

std::vector<std::string> tokenize(std::string_view text) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  std::string current;
  const auto flush = [&] {
    if (current.size() >= 2 && seen.insert(current).second) out.push_back(current);
    current.clear();
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (!is_alnum(c)) {
      flush();
      continue;
    }
    // Start a new token at camelCase boundaries: "parseHTTP" -> parse, http.
    const bool boundary =
        is_upper(c) && !current.empty() &&
        (is_lower(text[i - 1]) || (i + 1 < text.size() && is_lower(text[i + 1])));
    if (boundary) flush();
    current += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  flush();
  return out;
}

std::array<float, kVectorDims> embed(const std::vector<std::string>& tokens,
                                     float weight) {
  std::array<float, kVectorDims> v{};
  for (const std::string& t : tokens) {
    const std::uint64_t h = fnv1a(t);
    const std::size_t dim = static_cast<std::size_t>(h % kVectorDims);
    const float sign = ((h >> 32) & 1U) != 0 ? 1.0F : -1.0F;
    v[dim] += sign * weight;
  }
  normalise(v);
  return v;
}

void accumulate(std::array<float, kVectorDims>& into,
                const std::array<float, kVectorDims>& other) {
  for (std::size_t i = 0; i < kVectorDims; ++i) into[i] += other[i];
}

void normalise(std::array<float, kVectorDims>& v) {
  float sum = 0.0F;
  for (const float x : v) sum += x * x;
  if (sum == 0.0F) return;
  const float inv = 1.0F / std::sqrt(sum);
  for (float& x : v) x *= inv;
}

float dot(const std::array<float, kVectorDims>& a,
          const std::array<float, kVectorDims>& b) {
  float sum = 0.0F;
  for (std::size_t i = 0; i < kVectorDims; ++i) sum += a[i] * b[i];
  return sum;
}

}  // namespace veylqora
