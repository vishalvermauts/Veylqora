// M1.2 ingestion: small, empty, large, binary, invalid text, symlinks, renames,
// deletions, .gitignore handling, interrupted runs, and unreadable files.

#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/ingestion/process.hpp"
#include "veylqora/storage/database.hpp"

#include <atomic>
#include <fstream>
#include <string>
#include <string_view>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using veylqora::test::TempDir;

namespace {

void write(const fs::path& p, std::string_view content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << content;
}

bool git_available() {
  return veylqora::run_git(fs::temp_directory_path(), {"--version"}).has_value();
}

const veylqora::SystemClock kClock;

}  // namespace

TEST_CASE("empty directory indexes to nothing without error") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_indexed == 0);
  CHECK(stats.files_removed == 0);
  CHECK_FALSE(stats.canceled);
}

TEST_CASE("empty python file is indexed with no symbols") {
  TempDir tmp;
  write(tmp.path() / "empty.py", "");
  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_indexed == 1);
  CHECK(stats.symbols_written == 0);
}

TEST_CASE("large repository is processed in batches with progress") {
  TempDir tmp;
  for (int i = 0; i < 2000; ++i) {
    write(tmp.path() / "gen" / ("f_" + std::to_string(i) + ".py"),
          "def f_" + std::to_string(i) + "():\n    return " + std::to_string(i) + "\n");
  }
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::IndexOptions options;
  options.batch_size = 200;
  std::size_t reports = 0;
  std::size_t last_done = 0;
  options.on_progress = [&](std::size_t done, std::size_t total) {
    ++reports;
    last_done = done;
    CHECK(total == 2000);
  };
  const auto stats = veylqora::index_project(db, kClock, tmp.path(), options);
  CHECK(stats.files_indexed == 2000);
  CHECK(reports == 10);
  CHECK(last_done == 2000);
  CHECK(veylqora::find_symbols(db, tmp.path(), "f_1999").size() == 1);
}

TEST_CASE("binary file with a .py name is skipped, not parsed") {
  TempDir tmp;
  const std::string text = "def hidden():\n    pass\n";
  write(tmp.path() / "blob.py", text + std::string(2, '\0'));
  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_skipped == 1);
  CHECK(stats.files_indexed == 0);
  CHECK(veylqora::find_symbols(db, tmp.path(), "hidden").empty());
}

TEST_CASE("invalid UTF-8 is skipped and does not stop the run") {
  TempDir tmp;
  write(tmp.path() / "bad.py", "def broken():\n    return '\xFF'\n");
  write(tmp.path() / "good.py", "def fine():\n    return 1\n");
  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_skipped == 1);
  CHECK(stats.files_indexed == 1);
  CHECK(veylqora::find_symbols(db, tmp.path(), "broken").empty());
  CHECK(veylqora::find_symbols(db, tmp.path(), "fine").size() == 1);
}

TEST_CASE("valid multi-byte UTF-8 is indexed") {
  TempDir tmp;
  write(tmp.path() / "unicode.py",
        "# caf\xC3\xA9\ndef price():\n    return '\xE2\x82\xAC'\n");
  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_indexed == 1);
  CHECK(veylqora::find_symbols(db, tmp.path(), "price").size() == 1);
}

TEST_CASE("symlinks are never followed, even to files outside the project") {
  TempDir outside;
  TempDir tmp;
  write(outside.path() / "secret.py", "def leaked():\n    pass\n");
  write(tmp.path() / "inside.py", "def local():\n    pass\n");
  fs::create_symlink(outside.path() / "secret.py", tmp.path() / "link.py");
  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_indexed == 1);
  CHECK(veylqora::find_symbols(db, tmp.path(), "leaked").empty());
  CHECK(veylqora::find_symbols(db, tmp.path(), "local").size() == 1);
}

TEST_CASE("renamed file: old path disappears, new path is indexed") {
  TempDir tmp;
  write(tmp.path() / "old_name.py", "def moved():\n    pass\n");
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, tmp.path());
  REQUIRE(veylqora::find_symbols(db, tmp.path(), "moved").size() == 1);

  fs::rename(tmp.path() / "old_name.py", tmp.path() / "new_name.py");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_removed == 1);
  CHECK(stats.files_indexed == 1);
  const auto hits = veylqora::find_symbols(db, tmp.path(), "moved");
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].path == "new_name.py");
}

TEST_CASE("deleted file: its symbols disappear and the removal is counted") {
  TempDir tmp;
  write(tmp.path() / "gone.py", "def vanish():\n    pass\n");
  write(tmp.path() / "stay.py", "def remain():\n    pass\n");
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, tmp.path());

  fs::remove(tmp.path() / "gone.py");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_removed == 1);
  CHECK(veylqora::find_symbols(db, tmp.path(), "vanish").empty());
  CHECK(veylqora::find_symbols(db, tmp.path(), "remain").size() == 1);
}

TEST_CASE(
    "git repository: .gitignore is honoured, untracked-but-not-ignored files are kept") {
  if (!git_available()) {
    WARN("git not available; skipping");
    return;
  }
  TempDir tmp;
  REQUIRE(veylqora::run_git(tmp.path(), {"init", "-q"})->exit_code == 0);
  write(tmp.path() / ".gitignore", "secret.py\nignored/\n");
  write(tmp.path() / "keep.py", "def kept():\n    pass\n");
  write(tmp.path() / "secret.py", "def hidden_secret():\n    pass\n");
  write(tmp.path() / "ignored" / "x.py", "def in_ignored_dir():\n    pass\n");

  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_indexed == 1);
  CHECK(veylqora::find_symbols(db, tmp.path(), "kept").size() == 1);
  CHECK(veylqora::find_symbols(db, tmp.path(), "hidden_secret").empty());
  CHECK(veylqora::find_symbols(db, tmp.path(), "in_ignored_dir").empty());
}

TEST_CASE("interrupted run keeps committed batches and does not remove anything") {
  TempDir tmp;
  for (int i = 0; i < 10; ++i) {
    write(tmp.path() / ("m_" + std::to_string(i) + ".py"),
          "def m_" + std::to_string(i) + "():\n    pass\n");
  }
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, tmp.path());
  // Change every remaining file so the interrupted run has real work to commit,
  // and remove one so there is a removal the interrupted run must not apply.
  for (int i = 1; i < 10; ++i) {
    write(tmp.path() / ("m_" + std::to_string(i) + ".py"),
          "def m_" + std::to_string(i) + "():\n    return 2\n");
  }
  fs::remove(tmp.path() / "m_0.py");

  std::atomic<bool> cancel{false};
  veylqora::IndexOptions options;
  options.batch_size = 2;
  options.cancel = &cancel;
  options.on_progress = [&](std::size_t done, std::size_t) {
    if (done >= 4) cancel.store(true);
  };
  const auto partial = veylqora::index_project(db, kClock, tmp.path(), options);
  CHECK(partial.canceled);
  CHECK(partial.files_indexed == 4);  // exactly the first two batches were committed
  CHECK(partial.files_removed == 0);
  CHECK(veylqora::find_symbols(db, tmp.path(), "m_0").size() == 1);  // not removed yet

  // Re-running with no cancellation completes the work and applies the removal.
  const auto resumed = veylqora::index_project(db, kClock, tmp.path());
  CHECK_FALSE(resumed.canceled);
  CHECK(resumed.files_removed == 1);
  CHECK(resumed.files_indexed == 5);    // the work the interrupted run did not reach
  CHECK(resumed.files_unchanged == 4);  // the work it did commit is not repeated
  CHECK(veylqora::find_symbols(db, tmp.path(), "m_0").empty());
  CHECK(veylqora::find_symbols(db, tmp.path(), "m_9").size() == 1);
}

TEST_CASE("unreadable file keeps its previously indexed symbols") {
  TempDir tmp;
  const fs::path file = tmp.path() / "locked.py";
  write(file, "def stored():\n    pass\n");
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, tmp.path());

  fs::permissions(file, fs::perms::none);
  if (std::ifstream(file).good()) {
    fs::permissions(file, fs::perms::owner_all);
    WARN("file is still readable (running as root?); skipping");
    return;
  }
  const auto stats = veylqora::index_project(db, kClock, tmp.path());
  CHECK(stats.files_unreadable == 1);
  CHECK(stats.files_removed == 0);
  CHECK(veylqora::find_symbols(db, tmp.path(), "stored").size() == 1);
  fs::permissions(file, fs::perms::owner_all);
}
