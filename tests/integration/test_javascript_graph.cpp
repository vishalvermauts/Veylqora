// M1.3b JavaScript and TypeScript graph (ADR-0019). Each language has its own
// labelled fixture and is scored on its own; no shared fixture blends the numbers.

#include "veylqora/core/project.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/storage/database.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using veylqora::test::TempDir;

namespace {

// Fixed in ADR-0019 before the fixtures were scored.
constexpr double kMinPrecision = 0.90;
constexpr double kMinRecall = 0.85;

const fs::path kTypeScript = VEYLQORA_FIXTURE_DIR "/typescript";
const fs::path kJavaScript = VEYLQORA_FIXTURE_DIR "/javascript";
const veylqora::SystemClock kClock;

// Every "src -> dst" pair the graph claims a call or inheritance for, where the
// target is a project symbol. Unresolved names make no claim and are not scored.
std::set<std::string> emitted_edges(veylqora::Database& db, const fs::path& root) {
  std::set<std::string> out;
  veylqora::Statement q(db,
                        "SELECT s.fqn, d.fqn FROM edge e "
                        "JOIN symbol s ON s.id = e.src_symbol_id "
                        "JOIN symbol d ON d.id = e.dst_symbol_id "
                        "WHERE e.project_id = ? AND e.kind IN ('calls', 'inherits')");
  q.bind(1, veylqora::project_id_for(root));
  while (q.step()) out.insert(q.text(0) + " -> " + q.text(1));
  return out;
}

struct Score {
  std::size_t emitted;
  std::size_t expected;
  std::size_t correct;
  double precision() const {
    return emitted == 0 ? 0.0
                        : static_cast<double>(correct) / static_cast<double>(emitted);
  }
  double recall() const {
    return expected == 0 ? 0.0
                         : static_cast<double>(correct) / static_cast<double>(expected);
  }
};

Score score(const std::set<std::string>& emitted, const std::set<std::string>& expected) {
  std::size_t correct = 0;
  for (const std::string& e : emitted) {
    if (expected.contains(e)) ++correct;
  }
  return Score{emitted.size(), expected.size(), correct};
}

void report(const char* language, const Score& s, const std::set<std::string>& emitted,
            const std::set<std::string>& expected) {
  std::cout << language << ": precision " << s.precision() << " (" << s.correct << "/"
            << s.emitted << "), recall " << s.recall() << " (" << s.correct << "/"
            << s.expected << ")\n";
  for (const std::string& e : emitted) {
    if (!expected.contains(e)) std::cout << "  false positive: " << e << '\n';
  }
  for (const std::string& e : expected) {
    if (!emitted.contains(e)) std::cout << "  missed: " << e << '\n';
  }
}

// The labelled dataset. Every pair was checked by reading the fixture source.
const std::set<std::string> kTypeScriptLabels = {
    "auth.tokens.verifyToken -> auth.tokens.checkSignature",
    "auth.tokens.TokenStore.save -> auth.tokens.TokenStore.persist",
    "api.routes.handle -> auth.tokens.verifyToken",
    "api.routes.handle -> auth.tokens.TokenStore",
    "api.routes.handle -> auth.tokens.TokenStore.save",  // heuristic: name match
    "api.routes.handle -> util.text.slugify",
    "util.text.slugify -> util.text.normalise",
    "billing.invoice.Invoice.amount -> billing.base.BaseInvoice.total",  // heuristic
    "billing.invoice.Invoice -> billing.base.BaseInvoice",               // inherits
};

const std::set<std::string> kJavaScriptLabels = {
    "lib.geometry.area -> lib.geometry.multiply",
    "lib.geometry.Shape.describe -> lib.geometry.Shape.label",
    "lib.geometry.Square.side -> lib.geometry.area",
    "app.main.run -> lib.geometry.Square",
    "app.main.run -> lib.geometry.area",
    "app.main.run -> lib.geometry.Square.side",   // heuristic: name match
    "lib.geometry.Square -> lib.geometry.Shape",  // inherits
};

}  // namespace

TEST_CASE("TypeScript graph meets the labelled gate (ADR-0019)") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kTypeScript);
  const auto emitted = emitted_edges(db, kTypeScript);
  const Score s = score(emitted, kTypeScriptLabels);
  report("TypeScript", s, emitted, kTypeScriptLabels);
  CHECK(s.precision() >= kMinPrecision);
  CHECK(s.recall() >= kMinRecall);
}

TEST_CASE("JavaScript graph meets the labelled gate (ADR-0019)") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kJavaScript);
  const auto emitted = emitted_edges(db, kJavaScript);
  const Score s = score(emitted, kJavaScriptLabels);
  report("JavaScript", s, emitted, kJavaScriptLabels);
  CHECK(s.precision() >= kMinPrecision);
  CHECK(s.recall() >= kMinRecall);
}

TEST_CASE("a TypeScript file with a syntax error keeps its valid definitions") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, kTypeScript);
  CHECK(stats.files_with_syntax_errors >= 1);

  veylqora::Statement q(
      db,
      "SELECT COUNT(*) FROM symbol s JOIN project p ON p.id = s.project_id "
      "WHERE p.id = ? AND s.fqn = 'broken.partial.ok'");
  q.bind(1, veylqora::project_id_for(kTypeScript));
  q.step();
  CHECK(q.integer(0) == 1);
}

TEST_CASE("JavaScript files are recorded with the javascript language") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kJavaScript);
  veylqora::index_project(db, kClock, kTypeScript);
  veylqora::Statement q(db,
                        "SELECT DISTINCT f.language FROM file f JOIN project p "
                        "ON p.id = f.project_id WHERE p.id = ? ORDER BY f.language");
  q.bind(1, veylqora::project_id_for(kJavaScript));
  CHECK(q.step());
  CHECK(q.text(0) == "javascript");
}
