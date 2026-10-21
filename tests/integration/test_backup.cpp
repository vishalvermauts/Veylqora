// M3.4 backup and restore: a backup is a checked, consistent snapshot; a restore
// replaces a database only from a backup that passes its checks (ADR-0004).

#include "veylqora/api/service.hpp"
#include "veylqora/core/error.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/storage/backup.hpp"
#include "veylqora/storage/database.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using veylqora::test::TempDir;
namespace api = veylqora::api;
using api::Json;

namespace {

const fs::path kRoot = VEYLQORA_FIXTURE_DIR "/python";

Json remember(veylqora::Database& db, const veylqora::Clock& clock,
              veylqora::IdGenerator& ids, const std::string& content) {
  const Json body = {{"type", "decision"},
                     {"content", content},
                     {"author", "backup-test"},
                     {"scope", "shared"}};
  return api::memory_create(db, clock, ids, kRoot, body);
}

}  // namespace

TEST_CASE("a backup restores to the same index, memories and history") {
  TempDir tmp;
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  const fs::path live = tmp.path() / "live.db";
  const fs::path snapshot = tmp.path() / "snapshot.db";
  const fs::path restored = tmp.path() / "restored.db";

  Json before;
  {
    veylqora::Database db(live);
    api::index(db, clock, kRoot);
    remember(db, clock, ids, "first decision");
    remember(db, clock, ids, "second decision");
    veylqora::backup_database(db, snapshot);
    before = api::status(db, kRoot);
  }

  veylqora::restore_database(snapshot, restored);
  veylqora::Database copy(restored);
  const Json after = api::status(copy, kRoot);
  CHECK(after.at("files") == before.at("files"));
  CHECK(after.at("symbols") == before.at("symbols"));
  CHECK(after.at("memories") == 2);
  CHECK(api::doctor(copy).at("integrity") == "ok");
}

TEST_CASE("a backup is a snapshot: later writes to the live database are not in it") {
  TempDir tmp;
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  const fs::path snapshot = tmp.path() / "snapshot.db";
  veylqora::Database db(tmp.path() / "live.db");
  remember(db, clock, ids, "before the backup");
  veylqora::backup_database(db, snapshot);
  remember(db, clock, ids, "after the backup");

  veylqora::Database check(snapshot);
  CHECK(api::status(check, kRoot).at("memories") == 1);
}

TEST_CASE("backup refuses to overwrite an existing file") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "live.db");
  const fs::path dest = tmp.path() / "existing.db";
  {
    std::ofstream out(dest);
    out << "keep me";
  }
  CHECK_THROWS_AS(veylqora::backup_database(db, dest), veylqora::Error);
  std::ifstream in(dest);
  std::string text;
  std::getline(in, text);
  CHECK(text == "keep me");
}

TEST_CASE("a damaged backup is refused and the target database is left unchanged") {
  TempDir tmp;
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  const fs::path target = tmp.path() / "target.db";
  {
    veylqora::Database db(target);
    remember(db, clock, ids, "keep this memory");
  }
  const fs::path damaged = tmp.path() / "damaged.db";
  {
    std::ofstream out(damaged, std::ios::binary);
    out << "this is not a database";
  }

  CHECK_THROWS_AS(veylqora::restore_database(damaged, target), veylqora::Error);
  veylqora::Database still(target);
  CHECK(api::status(still, kRoot).at("memories") == 1);
}
