#pragma once

#include "veylqora/storage/database.hpp"

#include <filesystem>

namespace veylqora {

/// Writes a consistent copy of `source` to `dest` with SQLite's online backup, so
/// the copy is a snapshot even if the database has changes in its WAL. `dest`
/// must not exist. The copy is checked before the call returns, and removed if
/// it fails the check (M3.4).
void backup_database(Database& source, const std::filesystem::path& dest);

/// Replaces the contents of `target` with the backup at `backup`. The backup is
/// checked first, so a damaged file leaves `target` untouched. Callers hold the
/// database lock (ADR-0004), so no daemon is writing while this runs.
void restore_database(const std::filesystem::path& backup,
                      const std::filesystem::path& target);

}  // namespace veylqora
