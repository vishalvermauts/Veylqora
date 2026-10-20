// M1.5 API and daemon: the service layer, the HTTP server on a real port, the
// status codes, the loopback rule, and the single-writer file lock.

#include "veylqora/api/service.hpp"
#include "veylqora/core/file_lock.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/server/server.hpp"
#include "veylqora/storage/database.hpp"

#include <chrono>
#include <filesystem>
#include <httplib.h>
#include <thread>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using veylqora::test::TempDir;
namespace api = veylqora::api;
using api::Json;

namespace {

const fs::path kFixtures = VEYLQORA_FIXTURE_DIR "/python";

// Runs a server on a free port for the lifetime of the object.
class LiveServer {
 public:
  LiveServer(veylqora::Database& db, veylqora::Clock& clock, veylqora::IdGenerator& ids)
      : server_(db, clock, ids) {
    port_ = server_.bind("127.0.0.1", 0);
    thread_ = std::thread([this] { server_.serve(); });
    // serve() starts listening on its own thread; wait until it answers.
    httplib::Client probe("http://127.0.0.1:" + std::to_string(port_));
    for (int i = 0; i < 200; ++i) {
      if (probe.Get("/api/v1/health")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ~LiveServer() {
    server_.stop();
    thread_.join();
  }
  int port() const { return port_; }

 private:
  veylqora::Server server_;
  int port_ = 0;
  std::thread thread_;
};

}  // namespace

TEST_CASE("service: status, index, search and graph agree with the library") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  CHECK(api::status(db, kFixtures)["indexed"] == false);

  const Json stats = api::index(db, clock, kFixtures);
  CHECK(stats["indexed"] == 10);
  CHECK(api::status(db, kFixtures)["indexed"] == true);

  const Json hits = api::search(db, kFixtures, "verify_token");
  REQUIRE(hits["hits"].size() == 1);
  CHECK(hits["hits"][0]["path"] == "auth/tokens.py");
  CHECK(hits["hits"][0]["fqn"] == "auth.tokens.verify_token");

  const Json g = api::graph(db, kFixtures, "Base");
  CHECK(g["name"] == "Base");
}

TEST_CASE("service: memory operations and their error kinds") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;

  const Json created = api::memory_create(
      db, clock, ids, kFixtures,
      Json{{"type", "decision"}, {"content", "use bcrypt"}, {"author", "agent-a"}, {"scope", "shared"}});
  const std::string id = created["id"];
  CHECK(created["revision"] == 1);

  CHECK_THROWS_AS(api::memory_inspect(db, kFixtures, "missing"), veylqora::NotFoundError);
  CHECK_THROWS_AS(api::memory_create(db, clock, ids, kFixtures,
                                     Json{{"type", "decision"}, {"content", "x"}}),
                  veylqora::Error);  // author missing: invalid, not not-found

  const Json updated = api::memory_update(db, clock, kFixtures, id,
                                          Json{{"expected_revision", 1},
                                               {"content", "use argon2"},
                                               {"actor", "agent-b"},
                                               {"reason", "policy"}});
  CHECK(updated["revision"] == 2);
  CHECK_THROWS_AS(api::memory_update(db, clock, kFixtures, id,
                                     Json{{"expected_revision", 1},
                                          {"content", "stale"},
                                          {"actor", "agent-c"},
                                          {"reason", "late"}}),
                  veylqora::ConflictError);

  const Json inspected = api::memory_inspect(db, kFixtures, id);
  CHECK(inspected["history"].size() == 2);
}

TEST_CASE("file lock: a second holder is refused while the first is alive") {
  TempDir tmp;
  const fs::path lock_file = tmp.path() / "v.db.lock";
  {
    veylqora::FileLock first(lock_file);
    CHECK_THROWS_AS(veylqora::FileLock(lock_file), veylqora::Error);
  }
  CHECK_NOTHROW(
      veylqora::FileLock(lock_file));  // released when the first goes out of scope
}

TEST_CASE("server: refuses a non-loopback bind") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  veylqora::Server server(db, clock, ids);
  CHECK_THROWS_AS(server.bind("0.0.0.0", 0), veylqora::Error);
  CHECK_THROWS_AS(server.bind("192.168.1.10", 0), veylqora::Error);
}

TEST_CASE("server: JSON over HTTP, with stable error codes") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  LiveServer live(db, clock, ids);
  httplib::Client client("http://127.0.0.1:" + std::to_string(live.port()));

  const auto health = client.Get("/api/v1/health");
  REQUIRE(health);
  CHECK(health->status == 200);
  CHECK(Json::parse(health->body)["ok"] == true);

  const std::string root = kFixtures.string();
  const auto indexed =
      client.Post("/api/v1/index", Json{{"root", root}}.dump(), "application/json");
  REQUIRE(indexed);
  CHECK(indexed->status == 200);
  CHECK(Json::parse(indexed->body)["indexed"] == 10);

  const auto search =
      client.Get("/api/v1/search", httplib::Params{{"root", root}, {"name", "health"}});
  REQUIRE(search);
  CHECK(Json::parse(search->body)["hits"].size() == 1);

  const auto created = client.Post("/api/v1/memories",
                                   Json{{"root", root},
                                        {"type", "decision"},
                                        {"content", "http memory"},
                                        {"author", "agent-h"},
                                        {"scope", "shared"}}
                                       .dump(),
                                   "application/json");
  REQUIRE(created);
  const std::string id = Json::parse(created->body)["id"];

  const auto missing =
      client.Get("/api/v1/memories/nope", httplib::Params{{"root", root}});
  REQUIRE(missing);
  CHECK(missing->status == 404);
  CHECK(Json::parse(missing->body)["error"]["code"] == "not_found");

  const auto stale = client.Post("/api/v1/memories/" + id + "/update",
                                 Json{{"root", root},
                                      {"expected_revision", 9},
                                      {"content", "x"},
                                      {"actor", "a"},
                                      {"reason", "r"}}
                                     .dump(),
                                 "application/json");
  REQUIRE(stale);
  CHECK(stale->status == 409);
  CHECK(Json::parse(stale->body)["error"]["code"] == "conflict");

  const auto malformed = client.Post("/api/v1/index", "{not json", "application/json");
  REQUIRE(malformed);
  CHECK(malformed->status == 400);
  CHECK(Json::parse(malformed->body)["error"]["code"] == "invalid");

  const auto ok_update = client.Post("/api/v1/memories/" + id + "/update",
                                     Json{{"root", root},
                                          {"expected_revision", 1},
                                          {"content", "http v2"},
                                          {"actor", "a"},
                                          {"reason", "r"}}
                                         .dump(),
                                     "application/json");
  REQUIRE(ok_update);
  CHECK(ok_update->status == 200);
  CHECK(Json::parse(ok_update->body)["revision"] == 2);
}
