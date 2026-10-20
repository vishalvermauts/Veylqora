#pragma once

#include "veylqora/storage/database.hpp"

#include <cstddef>
#include <filesystem>

namespace veylqora {

/// Writes the project's memories, with history and evidence, as JSON Lines
/// (one record per line, ordered deterministically). Returns the memory count.
std::size_t export_memories(Database& db, const std::filesystem::path& root,
                            const std::filesystem::path& out_file);

/// Reads an export back into `db`, all or nothing. Returns the memory count.
/// Vector data is not part of the format: it is derived and rebuilt later.
std::size_t import_memories(Database& db, const std::filesystem::path& in_file);

}  // namespace veylqora
