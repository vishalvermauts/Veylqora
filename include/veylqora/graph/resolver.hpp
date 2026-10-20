#pragma once

#include "veylqora/storage/database.hpp"

#include <string>

namespace veylqora {

/// Rebuilds every edge of one project from its stored facts and symbols.
///
/// Parsing is per file; resolution is per project, because a name can refer to
/// a symbol in another file. Only stored rows are read, so no file is parsed
/// here. Resolution levels follow ADR-0010: resolved, heuristic, unresolved.
void rebuild_graph(Database& db, const std::string& project_id);

}  // namespace veylqora
