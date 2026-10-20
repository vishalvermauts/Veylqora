#pragma once

#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/memory/memory.hpp"
#include "veylqora/storage/database.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace veylqora::api {

using Json = nlohmann::json;

// Every operation the CLI and the HTTP server offer. Each takes its inputs as
// plain values or a JSON body and returns a JSON document. Errors are thrown as
// veylqora::Error, NotFoundError or ConflictError; transports map them to codes.

Json health();
Json status(Database& db, const std::filesystem::path& root);
Json index(Database& db, const Clock& clock, const std::filesystem::path& root,
           const IndexOptions& options = {});
Json search(Database& db, const std::filesystem::path& root, std::string_view name);
Json graph(Database& db, const std::filesystem::path& root, std::string_view name);

Json memory_create(Database& db, const Clock& clock, IdGenerator& ids,
                   const std::filesystem::path& root, const Json& body);
Json memory_list(Database& db, const std::filesystem::path& root, std::string_view state);
Json memory_inspect(Database& db, const std::filesystem::path& root, std::string_view id);
Json memory_update(Database& db, const Clock& clock, const std::filesystem::path& root,
                   std::string_view id, const Json& body);
Json memory_transition(Database& db, const Clock& clock, const std::filesystem::path& root,
                       std::string_view id, const Json& body);

Json doctor(Database& db);

}  // namespace veylqora::api
