// M2.2 index maintenance: consistency after insert, update, delete and interrupted
// runs; incremental updates compared with a full rebuild; updated and deleted
// definitions behave correctly in retrieval.

#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/retrieval/search.hpp"
#include "veylqora/storage/database.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using veylqora::test::TempDir;

namespace {

const fs::path kFixtures = VEYLQORA_FIXTURE_DIR "/python";
const veylqora::SystemClock kClock;

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

// Every (fqn, score) a set of queries returns, in order: a fingerprint of the
// retrieval state that two indexes must agree on exactly.
std::vector<std::string> snapshot(veylqora::Database& db, const fs::path& root) {
  std::vector<std::string> out;
  for (const char* q : {"token verification", "password hashing", "format money", "tax rate",
                        "health route", "slugify text", "measure timing"}) {
    for (const veylqora::Mode mode :
         {veylqora::Mode::lexical, veylqora::Mode::vector, veylqora::Mode::hybrid}) {
      veylqora::RetrievalOptions o;
      o.mode = mode;
      for (const veylqora::Hit& h : veylqora::retrieve(db, root, q, o)) {
        out.push_back(h.fqn + "|" + std::to_string(h.score));
      }
    }
  }
  return out;
}

}  // namespace

TEST_CASE("the index is consistent after a fresh index") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kFixtures);
  const auto c = veylqora::check_consistency(db, kFixtures);
  CHECK(c.ok());
  CHECK(c.symbols > 0);
}

TEST_CASE("consistency holds after edits and deletions") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  fs::copy(kFixtures, src, fs::copy_options::recursive);
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, src);

  write(src / "util" / "text.py", "def slugify(value):\n    return value\n\n\ndef brand_new():\n    return 1\n");
  fs::remove(src / "billing" / "tax.py");
  veylqora::index_project(db, kClock, src);

  CHECK(veylqora::check_consistency(db, src).ok());
  for (const auto& h : veylqora::retrieve(db, src, "tax rate")) CHECK(h.path != "billing/tax.py");
  CHECK(veylqora::retrieve(db, src, "brand new", {veylqora::Mode::lexical, 10, "", ""}).size() >= 1);
}

TEST_CASE("an incremental update gives the same retrieval as a full rebuild") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  fs::copy(kFixtures, src, fs::copy_options::recursive);
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, src);

  // Change three files, then update incrementally.
  write(src / "auth" / "tokens.py", "def verify_token(token):\n    return bool(token)\n");
  write(src / "api" / "routes.py", "def health():\n    return {\"status\": \"up\"}\n");
  write(src / "util" / "timing.py", "def measure(fn):\n    return fn\n");
  veylqora::index_project(db, kClock, src);
  const auto incremental = snapshot(db, src);

  veylqora::IndexOptions full;
  full.force = true;
  veylqora::index_project(db, kClock, src, full);
  CHECK(snapshot(db, src) == incremental);
  CHECK(veylqora::check_consistency(db, src).ok());
}

TEST_CASE("an interrupted update leaves a consistent index that a re-run completes") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  fs::copy(kFixtures, src, fs::copy_options::recursive);
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, src);
  const auto before = snapshot(db, src);

  // Every file changes, so the interrupted run has work to commit in batches.
  for (const char* f : {"auth/tokens.py", "auth/passwords.py", "billing/invoice.py", "billing/tax.py",
                        "users/models.py", "users/service.py", "api/routes.py", "api/middleware.py",
                        "util/text.py", "util/timing.py"}) {
    std::ofstream out(src / f, std::ios::app);
    out << "\n\n# touched\n";
  }
  std::atomic<bool> cancel{false};
  veylqora::IndexOptions options;
  options.batch_size = 2;
  options.cancel = &cancel;
  options.on_progress = [&](std::size_t done, std::size_t) {
    if (done >= 4) cancel.store(true);
  };
  const auto partial = veylqora::index_project(db, kClock, src, options);
  CHECK(partial.canceled);
  CHECK(veylqora::check_consistency(db, src).ok());  // never half a file

  veylqora::index_project(db, kClock, src);  // the re-run completes the update
  CHECK(veylqora::check_consistency(db, src).ok());
  CHECK(snapshot(db, src) == before);  // comment-only changes do not move any score
}
