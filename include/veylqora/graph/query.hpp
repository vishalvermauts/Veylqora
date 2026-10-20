#pragma once

#include "veylqora/storage/database.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace veylqora {

struct GraphEdge {
  std::string kind;        // calls, imports, inherits (contains is excluded from these queries)
  std::string src_fqn;
  std::string src_path;
  std::string dst_fqn;     // empty when unresolved
  std::string dst_hint;    // the name as written at the call site
  std::string resolution;  // resolved, heuristic, unresolved
  std::int64_t line;
};

struct Reached {
  std::string fqn;
  std::string path;
  std::size_t distance;  // edges from the start symbol, ignoring direction
};

/// Edges whose destination has this name: "what depends on X".
std::vector<GraphEdge> dependents_of(Database& db, const std::filesystem::path& root,
                                     std::string_view name);

/// Edges whose source has this name: "what does X depend on".
std::vector<GraphEdge> dependencies_of(Database& db, const std::filesystem::path& root,
                                       std::string_view name);

/// Symbols reachable from `name` within `hops` edges, in either direction over
/// resolved and heuristic edges. The start symbols are included at distance 0.
std::vector<Reached> neighbourhood(Database& db, const std::filesystem::path& root,
                                   std::string_view name, std::size_t hops);

/// Symbols in the given project-relative files, plus everything that directly
/// depends on them. This is the set a code change can put in doubt.
std::vector<Reached> change_impact(Database& db, const std::filesystem::path& root,
                                   const std::vector<std::string>& rel_paths);

}  // namespace veylqora
