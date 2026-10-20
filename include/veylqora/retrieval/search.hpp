#pragma once

#include "veylqora/storage/database.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace veylqora {

inline constexpr std::size_t kMaxResults = 100;
inline constexpr std::size_t kMaxQueryChars = 512;

enum class Mode : std::uint8_t {
  hybrid,   // lexical + vector, plus graph neighbours, fused by reciprocal rank
  lexical,  // identifier tokens only
  vector,   // hashed token vectors only (ADR-0015)
};

struct RetrievalOptions {
  Mode mode = Mode::hybrid;
  std::size_t top_k = 10;   // at most kMaxResults
  std::string kind;         // "", "function", "method" or "class"
  std::string path_prefix;  // "", or e.g. "auth/"
};

struct Hit {
  std::string fqn;
  std::string path;  // project-relative; this is the evidence reference
  std::string kind;
  std::int64_t line;
  double score;      // fused score; higher is better
};

/// Ranks the project's definitions against a free-text query. Deterministic:
/// the same index, query and options give the same list in the same order.
/// Throws Error for an empty or over-long query, or an out-of-range top_k.
std::vector<Hit> retrieve(Database& db, const std::filesystem::path& root, std::string_view query,
                          const RetrievalOptions& options = {});

}  // namespace veylqora
