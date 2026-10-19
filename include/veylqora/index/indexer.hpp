#pragma once

#include "veylqora/core/runtime.hpp"
#include "veylqora/storage/database.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace veylqora {

struct IndexStats {
  std::size_t files_indexed = 0;    // parsed, because new or changed
  std::size_t files_unchanged = 0;  // skipped, content hash matched
  std::size_t files_removed = 0;    // deleted from disk since the last run
  std::size_t symbols_written = 0;
};

/// Brings the database in line with the Python files under `root`, in one
/// transaction. Unchanged files are not re-parsed. Files that disappeared are
/// removed along with their symbols.
IndexStats index_project(Database& db, const Clock& clock, const std::filesystem::path& root);

struct SymbolHit {
  std::string path;  // project-relative, '/' separated
  std::string fqn;
  std::string kind;
  std::int64_t start_line;
};

/// Exact-name lookup within one project. Empty if the project is not indexed.
std::vector<SymbolHit> find_symbols(Database& db, const std::filesystem::path& root,
                                    std::string_view name);

}  // namespace veylqora
