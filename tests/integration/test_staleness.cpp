// M2.3 evidence-aware invalidation: a hand-labelled dataset of code changes, run
// through the real indexer and revalidation. Thresholds are fixed in ADR-0014 and
// are not adjusted to the results. Conflicting memories are not in the dataset:
// the system does not detect them (ADR-0014).

#include "veylqora/api/service.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/memory/memory.hpp"
#include "veylqora/storage/database.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using veylqora::test::TempDir;
namespace api = veylqora::api;

namespace {

constexpr double kMinPrecision = 0.80;  // ADR-0014
constexpr double kMinRecall = 0.70;     // ADR-0014

const std::string kAuthBase =
    "def verify(token):\n"
    "    return token.startswith(\"tok-\")\n"
    "\n"
    "\n"
    "def unrelated():\n"
    "    return 1\n";

const std::string kUtilBase = "def fmt(x):\n    return str(x)\n";

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

struct Scenario {
  std::string name;
  bool needs_attention;  // the label, decided before running
  std::function<void(const fs::path&)> change;
  std::function<void(const fs::path&)> revert;  // applied after the change, if set
  bool supersede_first = false;
};

struct Result {
  std::string name;
  bool label;
  bool flagged;
};

Result run(const Scenario& s) {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  write(src / "auth.py", kAuthBase);
  write(src / "util.py", kUtilBase);

  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  api::index(db, clock, src);

  veylqora::NewMemory m;
  m.type = "decision";
  m.content = "verify is the token check";
  m.author = "dataset";
  m.evidence = {{"auth.py", "auth.verify"}};
  const std::string id = veylqora::create_memory(db, clock, ids, src, m);

  if (s.supersede_first) {
    veylqora::NewMemory n = m;
    n.content = "a newer decision";
    n.evidence = {{"util.py", "util.fmt"}};
    const std::string newer = veylqora::create_memory(db, clock, ids, src, n);
    veylqora::supersede_memory(db, clock, src, id, 1, newer, "dataset", "replaced");
  }

  s.change(src);
  api::index(db, clock, src);
  if (s.revert) {
    s.revert(src);
    api::index(db, clock, src);
  }

  const auto memory = veylqora::get_memory(db, src, id);
  // "Flagged" means the memory needs attention. A superseded or invalidated
  // memory was decided on, not flagged by this check (ADR-0014).
  const bool flagged =
      memory->state == "suspected_stale" || memory->state == "evidence_missing";
  return Result{s.name, s.needs_attention, flagged};
}

}  // namespace

TEST_CASE("labelled dataset: precision and recall meet the gate (ADR-0014)") {
  const std::vector<Scenario> dataset = {
      {"cited body edited", true,
       [](const fs::path& src) {
         write(src / "auth.py",
               "def verify(token):\n    return bool(token)\n\n\ndef unrelated():\n    "
               "return 1\n");
       },
       nullptr},
      {"unrelated function edited", false,
       [](const fs::path& src) {
         write(src / "auth.py",
               "def verify(token):\n    return token.startswith(\"tok-\")\n\n\ndef "
               "unrelated():\n    return 2\n");
       },
       nullptr},
      {"comment added inside cited body", false,
       [](const fs::path& src) {
         write(src / "auth.py",
               "def verify(token):\n    # prefix is fixed\n    return "
               "token.startswith(\"tok-\")\n\n\ndef unrelated():\n    return 1\n");
       },
       nullptr},
      {"trailing whitespace only", false,
       [](const fs::path& src) {
         write(src / "auth.py",
               "def verify(token):\n    return token.startswith(\"tok-\")   \n\n\ndef "
               "unrelated():\n    return 1\n");
       },
       nullptr},
      {"renamed, body unchanged", false,
       [](const fs::path& src) {
         write(src / "auth.py",
               "def check(token):\n    return token.startswith(\"tok-\")\n\n\ndef "
               "unrelated():\n    return 1\n");
       },
       nullptr},
      {"moved to another file, body unchanged", false,
       [](const fs::path& src) {
         write(src / "auth.py", "def unrelated():\n    return 1\n");
         write(src / "util.py",
               kUtilBase +
                   "\n\ndef verify(token):\n    return token.startswith(\"tok-\")\n");
       },
       nullptr},
      {"signature changed", true,
       [](const fs::path& src) {
         write(src / "auth.py",
               "def verify(token, strict=False):\n    return "
               "token.startswith(\"tok-\")\n\n\ndef unrelated():\n    return 1\n");
       },
       nullptr},
      {"edited then reverted", false,
       [](const fs::path& src) {
         write(src / "auth.py",
               "def verify(token):\n    return bool(token)\n\n\ndef unrelated():\n    "
               "return 1\n");
       },
       [](const fs::path& src) { write(src / "auth.py", kAuthBase); }},
      {"cited symbol deleted", true,
       [](const fs::path& src) {
         write(src / "auth.py", "def unrelated():\n    return 1\n");
       },
       nullptr},
      {"superseded, then cited body edited", false,
       [](const fs::path& src) {
         write(src / "auth.py",
               "def verify(token):\n    return bool(token)\n\n\ndef unrelated():\n    "
               "return 1\n");
       },
       nullptr, true},
      {"unrelated file changed", false,
       [](const fs::path& src) {
         write(src / "util.py", "def fmt(x):\n    return repr(x)\n");
       },
       nullptr},
  };

  std::size_t tp = 0;
  std::size_t fp = 0;
  std::size_t fn = 0;
  std::size_t tn = 0;
  for (const Scenario& s : dataset) {
    const Result r = run(s);
    INFO("scenario: " << r.name << " label=" << r.label << " flagged=" << r.flagged);
    if (r.label && r.flagged) ++tp;
    if (!r.label && r.flagged) ++fp;
    if (r.label && !r.flagged) ++fn;
    if (!r.label && !r.flagged) ++tn;
    CHECK(r.flagged == r.label);
  }

  const double precision =
      (tp + fp) == 0 ? 0.0 : static_cast<double>(tp) / static_cast<double>(tp + fp);
  const double recall =
      (tp + fn) == 0 ? 0.0 : static_cast<double>(tp) / static_cast<double>(tp + fn);
  INFO("tp=" << tp << " fp=" << fp << " fn=" << fn << " tn=" << tn);
  INFO("precision=" << precision << " recall=" << recall);
  CHECK(precision >= kMinPrecision);
  CHECK(recall >= kMinRecall);
}

TEST_CASE("accepting a flagged memory returns it to active and keeps it there") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  write(src / "auth.py", kAuthBase);
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  api::index(db, clock, src);

  veylqora::NewMemory m;
  m.type = "decision";
  m.content = "verify is the token check";
  m.author = "dataset";
  m.evidence = {{"auth.py", "auth.verify"}};
  const std::string id = veylqora::create_memory(db, clock, ids, src, m);

  write(
      src / "auth.py",
      "def verify(token):\n    return bool(token)\n\n\ndef unrelated():\n    return 1\n");
  api::index(db, clock, src);
  REQUIRE(veylqora::get_memory(db, src, id)->state == "suspected_stale");

  veylqora::accept_memory(db, clock, src, id, 2, "reviewer", "checked the new check");
  CHECK(veylqora::get_memory(db, src, id)->state == "active");

  // The accepted fingerprint is now the current one, so a re-run changes nothing.
  const auto again = veylqora::revalidate_project(db, clock, src);
  CHECK(again.flagged == 0);
  CHECK(veylqora::get_memory(db, src, id)->state == "active");
}
