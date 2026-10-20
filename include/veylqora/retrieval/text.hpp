#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace veylqora {

/// Dimensions of the deterministic token-hash vectors (ADR-0015).
inline constexpr std::size_t kVectorDims = 256;

/// Identifier tokens: split on non-alphanumerics and at camelCase boundaries,
/// lower-cased, at least two characters, duplicates removed, in first-seen order.
std::vector<std::string> tokenize(std::string_view text);

/// L2-normalised vector of the tokens, each token weighted by `weight`. An
/// all-zero input gives an all-zero vector. Deterministic across platforms.
std::array<float, kVectorDims> embed(const std::vector<std::string>& tokens, float weight);

/// Adds `other` into `into`. Used to combine a symbol's name, qualified-name and
/// body vectors before normalising.
void accumulate(std::array<float, kVectorDims>& into, const std::array<float, kVectorDims>& other);

/// Scales to unit length in place; a zero vector is left unchanged.
void normalise(std::array<float, kVectorDims>& v);

float dot(const std::array<float, kVectorDims>& a, const std::array<float, kVectorDims>& b);

}  // namespace veylqora
