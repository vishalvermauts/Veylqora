// M1.3a code graph: a hand-labelled fixture (tests/fixtures/graph), precision and
// recall against that label set, query behaviour, updates, and isolation.

#include "veylqora/core/runtime.hpp"
#include "veylqora/graph/query.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/storage/database.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using veylqora::test::TempDir;

namespace {

const fs::path kGraph = VEYLQORA_FIXTURE_DIR "/graph";
const veylqora::SystemClock kClock;

// Written by hand from reading tests/fixtures/graph/*.py. Format: kind|fqn for
// symbols, kind|src|dst|resolution for edges. "-" means no resolved target.
const std::set<std::string> kLabelledSymbols = {
    "module|util",           "module|core",           "module|main",
    "function|util.helper",  "function|util.unused",  "class|core.Base",
    "method|core.Base.run",  "method|core.Base.step", "class|core.Child",
    "method|core.Child.run", "function|main.main",
};

const std::set<std::string> kLabelledEdges = {
    // contains: a module or class owns its definitions
    "contains|util|util.helper|resolved",
    "contains|util|util.unused|resolved",
    "contains|core|core.Base|resolved",
    "contains|core|core.Child|resolved",
    "contains|core.Base|core.Base.run|resolved",
    "contains|core.Base|core.Base.step|resolved",
    "contains|core.Child|core.Child.run|resolved",
    "contains|main|main.main|resolved",
    // imports
    "imports|core|util|resolved",
    "imports|main|core|resolved",
    // inherits
    "inherits|core.Child|core.Base|resolved",
    // calls
    "calls|core.Base.run|core.Base.step|resolved",   // self.step(), same class
    "calls|core.Base.step|util.helper|resolved",     // helper, bound by the import
    "calls|core.Child.run|core.Base.run|heuristic",  // super().run(): receiver unknown
    "calls|core.Child.run|core.Child.run|heuristic",
    "calls|core.Child.run|-|unresolved",        // super() is not defined in the project
    "calls|main.main|core.Child|resolved",      // Child(), bound by from-import
    "calls|main.main|core.Base|resolved",       // core.Base(), bound by import core
    "calls|main.main|core.Base.run|heuristic",  // c.run(): receiver unknown
    "calls|main.main|core.Child.run|heuristic",
    "calls|main.main|core.Base.step|heuristic",  // core.Base().step(): receiver is a call
    "calls|main.main|-|unresolved",              // len() is a builtin
};

struct Score {
  std::size_t correct;
  std::size_t reported;
  std::size_t expected;
  double precision() const {
    return reported == 0 ? 0.0
                         : static_cast<double>(correct) / static_cast<double>(reported);
  }
  double recall() const {
    return expected == 0 ? 0.0
                         : static_cast<double>(correct) / static_cast<double>(expected);
  }
};

Score score(const std::set<std::string>& reported,
            const std::set<std::string>& expected) {
  std::size_t correct = 0;
  for (const std::string& r : reported) correct += expected.count(r);
  return Score{correct, reported.size(), expected.size()};
}

std::set<std::string> actual_symbols(veylqora::Database& db) {
  std::set<std::string> out;
  veylqora::Statement q(db, "SELECT kind, fqn FROM symbol");
  while (q.step()) out.insert(q.text(0) + "|" + q.text(1));
  return out;
}

std::set<std::string> actual_edges(veylqora::Database& db) {
  std::set<std::string> out;
  veylqora::Statement q(
      db,
      "SELECT e.kind, s.fqn, COALESCE(d.fqn, '-'), e.resolution FROM edge e "
      "JOIN symbol s ON s.id = e.src_symbol_id "
      "LEFT JOIN symbol d ON d.id = e.dst_symbol_id");
  while (q.step())
    out.insert(q.text(0) + "|" + q.text(1) + "|" + q.text(2) + "|" + q.text(3));
  return out;
}

void write(const fs::path& p, std::string_view content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << content;
}

void copy_tree(const fs::path& from, const fs::path& to) {
  fs::create_directories(to);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

bool has_dependent(const std::vector<veylqora::GraphEdge>& edges,
                   std::string_view src_fqn, std::string_view resolution) {
  for (const auto& e : edges) {
    if (e.src_fqn == src_fqn && e.resolution == resolution) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("labelled fixture: symbol and edge precision and recall meet the gate") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kGraph);

  const Score symbols = score(actual_symbols(db), kLabelledSymbols);
  const Score edges = score(actual_edges(db), kLabelledEdges);

  INFO("symbols precision " << symbols.precision() << " recall " << symbols.recall());
  INFO("edges precision " << edges.precision() << " recall " << edges.recall());
  CHECK(symbols.precision() >= 0.95);
  CHECK(symbols.recall() >= 0.95);
  CHECK(edges.precision() >= 0.95);
  CHECK(edges.recall() >= 0.95);

  // Stated exactly, not only as a threshold: this fixture is small enough to
  // require a perfect match, and a regression should be visible at once.
  CHECK(symbols.correct == kLabelledSymbols.size());
  CHECK(edges.correct == kLabelledEdges.size());
}

TEST_CASE("every calls edge carries a resolution level") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kGraph);
  veylqora::Statement q(
      db,
      "SELECT COUNT(*) FROM edge WHERE kind = 'calls' AND resolution NOT IN "
      "('resolved', 'heuristic', 'unresolved')");
  REQUIRE(q.step());
  CHECK(q.integer(0) == 0);
}

TEST_CASE("reverse lookup: what depends on helper") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kGraph);
  const auto deps = veylqora::dependents_of(db, kGraph, "helper");
  CHECK(has_dependent(deps, "core.Base.step", "resolved"));
  CHECK(deps.size() == 1);
}

TEST_CASE("forward lookup: what main depends on") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kGraph);
  const auto deps = veylqora::dependencies_of(db, kGraph, "main");
  bool found_child = false;
  for (const auto& e : deps) {
    found_child = found_child || (e.kind == "calls" && e.dst_fqn == "core.Child" &&
                                  e.resolution == "resolved");
  }
  CHECK(found_child);
}

TEST_CASE("neighbourhood reaches symbols within the hop limit") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kGraph);
  const auto one = veylqora::neighbourhood(db, kGraph, "helper", 1);
  const auto two = veylqora::neighbourhood(db, kGraph, "helper", 2);

  std::set<std::string> fqns_one;
  for (const auto& r : one) fqns_one.insert(r.fqn);
  CHECK(fqns_one.count("util.helper") == 1);
  CHECK(fqns_one.count("core.Base.step") == 1);
  CHECK(fqns_one.count("core.Base.run") == 0);  // two hops away

  std::set<std::string> fqns_two;
  for (const auto& r : two) fqns_two.insert(r.fqn);
  CHECK(fqns_two.count("core.Base.run") == 1);
}

TEST_CASE("change impact: editing util.py puts its dependents in doubt") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, kGraph);
  const auto impact = veylqora::change_impact(db, kGraph, {"util.py"});
  bool helper_direct = false;
  bool step_dependent = false;
  for (const auto& r : impact) {
    if (r.fqn == "util.helper" && r.distance == 0) helper_direct = true;
    if (r.fqn == "core.Base.step" && r.distance == 1) step_dependent = true;
  }
  CHECK(helper_direct);
  CHECK(step_dependent);
}

TEST_CASE("deleting a file removes its symbols and the edges that pointed at them") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  copy_tree(kGraph, src);
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, src);
  REQUIRE(has_dependent(veylqora::dependents_of(db, src, "helper"), "core.Base.step",
                        "resolved"));

  fs::remove(src / "core.py");
  veylqora::index_project(db, kClock, src);
  CHECK(veylqora::find_symbols(db, src, "Base").empty());
  CHECK(veylqora::dependents_of(db, src, "helper").empty());
  CHECK(veylqora::dependents_of(db, src, "Base").empty());
  CHECK(veylqora::find_symbols(db, src, "helper").size() == 1);
}

TEST_CASE("editing a file rewires its edges, with no stale ones left behind") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  copy_tree(kGraph, src);
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, src);

  // Base.step stops calling helper and calls unused instead.
  write(src / "core.py",
        "from util import unused\n\n\nclass Base:\n    def run(self):\n        return "
        "self.step()\n\n"
        "    def step(self):\n        return unused()\n\n\nclass Child(Base):\n    def "
        "run(self):\n"
        "        return super().run()\n");
  veylqora::index_project(db, kClock, src);
  CHECK(veylqora::dependents_of(db, src, "helper").empty());
  CHECK(has_dependent(veylqora::dependents_of(db, src, "unused"), "core.Base.step",
                      "resolved"));
}

TEST_CASE("two projects in one database have separate graphs") {
  TempDir tmp;
  const fs::path a = tmp.path() / "a";
  const fs::path b = tmp.path() / "b";
  copy_tree(kGraph, a);
  copy_tree(kGraph, b);
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, a);
  veylqora::index_project(db, kClock, b);

  const auto in_a = veylqora::dependents_of(db, a, "helper");
  const auto in_b = veylqora::dependents_of(db, b, "helper");
  CHECK(in_a.size() == 1);
  CHECK(in_b.size() == 1);
  CHECK(veylqora::dependents_of(db, tmp.path(), "helper").empty());
}

TEST_CASE("a malformed file does not stop the rest of the graph from being built") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  copy_tree(kGraph, src);
  write(src / "broken.py", "def oops(:\n    return helper(\n");
  veylqora::Database db(tmp.path() / "v.db");
  const auto stats = veylqora::index_project(db, kClock, src);
  CHECK(stats.files_with_syntax_errors == 1);
  CHECK(has_dependent(veylqora::dependents_of(db, src, "helper"), "core.Base.step",
                      "resolved"));
}

TEST_CASE("re-exported names resolve to their defining module") {
  TempDir tmp;
  const fs::path src = tmp.path() / "src";
  // pkg/__init__.py re-exports Thing from pkg.core, as django does with QuerySet.
  write(src / "pkg" / "__init__.py", "from .core import Thing\n");
  write(src / "pkg" / "core.py", "class Thing:\n    pass\n");
  write(src / "app.py", "from pkg import Thing\n\n\nclass Use(Thing):\n    pass\n");
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::index_project(db, kClock, src);
  const auto deps = veylqora::dependents_of(db, src, "Thing");
  bool resolved_inherit = false;
  for (const auto& e : deps) {
    resolved_inherit =
        resolved_inherit || (e.kind == "inherits" && e.src_fqn == "app.Use" &&
                             e.resolution == "resolved" && e.dst_fqn == "pkg.core.Thing");
  }
  CHECK(resolved_inherit);
}
