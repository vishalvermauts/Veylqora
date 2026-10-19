// Vertical slice (M1.1 exit criterion): index a 10-file repository, search for
// symbols, close the database, reopen it, and check that the answers are the
// same. Also covers re-index, edits, deletions, and project isolation.

#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/storage/database.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = VEYLQORA_FIXTURE_DIR "/python";

// A temporary directory removed on destruction.
class TempDir {
 public:
  TempDir()
      : path_(fs::temp_directory_path() /
              ("veylqora-test-" + std::to_string(std::random_device{}()))) {
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

struct Expected {
  std::string name;
  std::string path;
  std::string fqn;
  std::string kind;
};

// Labelled by hand from the fixture sources, not taken from the program's output.
const std::vector<Expected> kExpected = {
    {"verify_token", "auth/tokens.py", "auth.tokens.verify_token", "function"},
    {"TokenStore", "auth/tokens.py", "auth.tokens.TokenStore", "class"},
    {"compute_total", "billing/invoice.py", "billing.invoice.Invoice.compute_total",
     "method"},
    {"Profile", "users/models.py", "users.models.User.Profile", "class"},
    {"describe", "users/models.py", "users.models.User.Profile.describe", "method"},
    {"normalise", "users/service.py", "users.service.UserService.create_user.normalise",
     "function"},
    {"health", "api/routes.py", "api.routes.health", "function"},
    {"__call__", "api/middleware.py", "api.middleware.RequestLogger.__call__", "method"},
    {"measure", "util/timing.py", "util.timing.measure", "function"},
};

void copy_tree(const fs::path& from, const fs::path& to) {
  fs::create_directories(to);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

}  // namespace

TEST_CASE("slice: index, search, restart, and get the same answers") {
  TempDir tmp;
  const fs::path db_file = tmp.path() / "veylqora.db";
  const veylqora::SystemClock clock;

  {
    veylqora::Database db(db_file);
    const auto stats = veylqora::index_project(db, clock, kFixtures);
    CHECK(stats.files_indexed == 10);
    CHECK(stats.files_unchanged == 0);
    CHECK(stats.files_removed == 0);
    CHECK(stats.symbols_written >= kExpected.size());
  }

  // Each block below is a new open of the same file, as after a restart.
  for (const Expected& e : kExpected) {
    veylqora::Database db(db_file);
    const auto hits = veylqora::find_symbols(db, kFixtures, e.name);
    INFO("name: " << e.name);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].path == e.path);
    CHECK(hits[0].fqn == e.fqn);
    CHECK(hits[0].kind == e.kind);
  }
}

TEST_CASE("slice: re-indexing an unchanged tree parses nothing") {
  TempDir tmp;
  const fs::path db_file = tmp.path() / "veylqora.db";
  const veylqora::SystemClock clock;
  veylqora::Database db(db_file);
  veylqora::index_project(db, clock, kFixtures);

  const auto again = veylqora::index_project(db, clock, kFixtures);
  CHECK(again.files_indexed == 0);
  CHECK(again.files_unchanged == 10);
  CHECK(veylqora::find_symbols(db, kFixtures, "verify_token").size() == 1);
}

TEST_CASE("slice: an edited file is re-parsed and its symbols replaced") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  copy_tree(kFixtures, src);
  const fs::path db_file = tmp.path() / "veylqora.db";
  const veylqora::SystemClock clock;
  veylqora::Database db(db_file);
  veylqora::index_project(db, clock, src);

  {
    std::ofstream out(src / "util" / "text.py", std::ios::app);
    out << "\n\ndef reverse(value):\n    return value[::-1]\n";
  }
  const auto stats = veylqora::index_project(db, clock, src);
  CHECK(stats.files_indexed == 1);
  CHECK(stats.files_unchanged == 9);
  CHECK(veylqora::find_symbols(db, src, "reverse").size() == 1);
  CHECK(veylqora::find_symbols(db, src, "slugify").size() == 1);
}

TEST_CASE("slice: a deleted file's symbols disappear from search") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  copy_tree(kFixtures, src);
  const fs::path db_file = tmp.path() / "veylqora.db";
  const veylqora::SystemClock clock;
  veylqora::Database db(db_file);
  veylqora::index_project(db, clock, src);
  REQUIRE(veylqora::find_symbols(db, src, "slugify").size() == 1);

  fs::remove(src / "util" / "text.py");
  const auto stats = veylqora::index_project(db, clock, src);
  CHECK(stats.files_removed == 1);
  CHECK(veylqora::find_symbols(db, src, "slugify").empty());
  CHECK(veylqora::find_symbols(db, src, "measure").size() == 1);
}

TEST_CASE("slice: projects sharing one database do not see each other") {
  TempDir tmp;
  const fs::path a = tmp.path() / "a";
  const fs::path b = tmp.path() / "b";
  copy_tree(kFixtures, a);
  copy_tree(kFixtures, b);
  const fs::path db_file = tmp.path() / "veylqora.db";
  const veylqora::SystemClock clock;
  veylqora::Database db(db_file);
  veylqora::index_project(db, clock, a);
  veylqora::index_project(db, clock, b);

  // Identical contents, so each project should return exactly its own hit.
  CHECK(veylqora::find_symbols(db, a, "verify_token").size() == 1);
  CHECK(veylqora::find_symbols(db, b, "verify_token").size() == 1);
  CHECK(veylqora::find_symbols(db, kFixtures, "verify_token").empty());
}

TEST_CASE("slice: unknown names and unknown projects return no hits") {
  TempDir tmp;
  const fs::path db_file = tmp.path() / "veylqora.db";
  const veylqora::SystemClock clock;
  veylqora::Database db(db_file);
  veylqora::index_project(db, clock, kFixtures);
  CHECK(veylqora::find_symbols(db, kFixtures, "no_such_symbol").empty());
  CHECK(veylqora::find_symbols(db, kFixtures, "").empty());
  CHECK(veylqora::find_symbols(db, tmp.path(), "verify_token").empty());
}
