#pragma once

#include "veylqora/core/runtime.hpp"
#include "veylqora/storage/database.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace veylqora {

struct IndexOptions {
  /// Re-parse every file even when its content hash is unchanged. This is the
  /// rebuild fallback (M2.2): it regenerates all rows from the source.
  bool force = false;
  /// Files per transaction. Bounds memory and how long a write lock is held,
  /// and is the granularity at which an interrupted run resumes.
  std::size_t batch_size = 200;
  /// Checked between files. Set it from a signal handler or another thread.
  const std::atomic<bool>* cancel = nullptr;
  /// Called after each committed batch with (files processed, files total).
  std::function<void(std::size_t, std::size_t)> on_progress;
};

struct IndexStats {
  std::size_t files_indexed = 0;    // parsed, because new or changed
  std::size_t files_unchanged = 0;  // skipped, content hash matched
  std::size_t files_removed =
      0;  // deleted from disk or from the project since the last run
  std::size_t files_skipped = 0;  // not indexable: binary, invalid UTF-8, too large
  std::size_t files_with_syntax_errors = 0;  // indexed partially, see the warning
  std::size_t files_unreadable = 0;  // I/O error; previously indexed rows were kept
  std::size_t symbols_written = 0;
  bool canceled = false;  // stopped early; re-running resumes from the stored state
};

/// Brings the database in line with the source files under `root`.
///
/// Work is committed in batches, so a cancelled or crashed run loses at most one
/// batch, and a re-run skips every file whose content is already stored. Files
/// that disappeared are removed only when the run completes, so a partial run
/// never deletes data it did not reach.
IndexStats index_project(Database& db, const Clock& clock,
                         const std::filesystem::path& root,
                         const IndexOptions& options = {});

/// Counts that must agree in a consistent index (M2.2). A non-zero orphan or
/// missing count means a bug or a corrupted database, not a normal state.
struct Consistency {
  std::size_t symbols = 0;
  std::size_t symbols_without_tokens = 0;
  std::size_t symbols_without_vectors = 0;
  std::size_t orphan_vectors = 0;  // vectors whose symbol is gone
  std::size_t orphan_tokens = 0;   // token rows whose symbol is gone
  bool ok() const {
    return symbols_without_tokens == 0 && symbols_without_vectors == 0 && orphan_vectors == 0 &&
           orphan_tokens == 0;
  }
};

/// Checks the retrieval rows against the symbols they describe, for one project.
Consistency check_consistency(Database& db, const std::filesystem::path& root);

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
