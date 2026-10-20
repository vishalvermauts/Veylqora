// M1.4 memory engine: CRUD, revision conflicts, state transitions, supersession,
// evidence validation and rollback, restart persistence, project isolation,
// retention, and the export/import round trip.

#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/memory/memory.hpp"
#include "veylqora/memory/transfer.hpp"
#include "veylqora/storage/database.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using veylqora::test::TempDir;

namespace {

const fs::path kFixtures = VEYLQORA_FIXTURE_DIR "/python";

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

veylqora::NewMemory decision(std::string content) {
  veylqora::NewMemory m;
  m.type = "decision";
  m.content = std::move(content);
  m.author = "agent-a";
  return m;
}

}  // namespace

TEST_CASE("create, read back and list a memory") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::FixedClock clock(veylqora::Timestamp{} + 1700000000s);
  veylqora::SequentialIds ids;

  const std::string id =
      veylqora::create_memory(db, clock, ids, kFixtures, decision("use bcrypt"));
  const auto m = veylqora::get_memory(db, kFixtures, id);
  REQUIRE(m.has_value());
  CHECK(m->content == "use bcrypt");
  CHECK(m->state == "active");
  CHECK(m->revision_no == 1);
  CHECK(m->author == "agent-a");
  CHECK(m->created_at == "2023-11-14T22:13:20Z");
  CHECK(veylqora::list_memories(db, kFixtures).size() == 1);
}

TEST_CASE("update with the current revision succeeds and keeps history") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  const std::string id =
      veylqora::create_memory(db, clock, ids, kFixtures, decision("v1"));

  veylqora::update_memory(db, clock, kFixtures, id, 1, "v2", "agent-a",
                          "tightened wording");
  CHECK(veylqora::get_memory(db, kFixtures, id)->revision_no == 2);

  const auto history = veylqora::revisions_of(db, kFixtures, id);
  REQUIRE(history.size() == 2);
  CHECK(history[0].content == "v1");
  CHECK(history[0].reason == "created");
  CHECK(history[1].content == "v2");
  CHECK(history[1].reason == "tightened wording");
}

TEST_CASE("a stale revision is a conflict and nothing is written") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  const std::string id =
      veylqora::create_memory(db, clock, ids, kFixtures, decision("base"));

  // Two writers both read revision 1. The first wins; the second must not overwrite it.
  veylqora::update_memory(db, clock, kFixtures, id, 1, "first writer", "agent-a", "a");
  CHECK_THROWS_AS(veylqora::update_memory(db, clock, kFixtures, id, 1, "second writer",
                                          "agent-b", "b"),
                  veylqora::ConflictError);
  const auto m = veylqora::get_memory(db, kFixtures, id);
  CHECK(m->content == "first writer");
  CHECK(m->revision_no == 2);
  CHECK(veylqora::revisions_of(db, kFixtures, id).size() ==
        2);  // no lost update, no phantom revision
}

TEST_CASE("concurrent writers from separate connections never lose an update") {
  TempDir tmp;
  const fs::path file = tmp.path() / "v.db";
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  std::string id;
  {
    veylqora::Database db(file);
    id = veylqora::create_memory(db, clock, ids, kFixtures, decision("start"));
  }

  // Two connections, each reading revision 1 before either writes.
  veylqora::Database a(file);
  veylqora::Database b(file);
  const auto seen_a = veylqora::get_memory(a, kFixtures, id)->revision_no;
  const auto seen_b = veylqora::get_memory(b, kFixtures, id)->revision_no;
  veylqora::update_memory(a, clock, kFixtures, id, seen_a, "from a", "agent-a", "a");
  CHECK_THROWS_AS(
      veylqora::update_memory(b, clock, kFixtures, id, seen_b, "from b", "agent-b", "b"),
      veylqora::ConflictError);
  CHECK(veylqora::get_memory(b, kFixtures, id)->content == "from a");
}

TEST_CASE(
    "state transitions follow the table; illegal ones and missing reasons are refused") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  const std::string id =
      veylqora::create_memory(db, clock, ids, kFixtures, decision("x"));

  veylqora::transition_memory(db, clock, kFixtures, id, 1, "suspected_stale", "system",
                              "code changed");
  CHECK(veylqora::get_memory(db, kFixtures, id)->state == "suspected_stale");

  // suspected_stale -> evidence_missing is not in the table, whatever the reason.
  CHECK_THROWS_AS(veylqora::transition_memory(db, clock, kFixtures, id, 2,
                                              "evidence_missing", "x", "why"),
                  veylqora::Error);
  CHECK_THROWS_AS(
      veylqora::transition_memory(db, clock, kFixtures, id, 2, "active", "x", ""),
      veylqora::Error);  // reason required
  CHECK(veylqora::get_memory(db, kFixtures, id)->state == "suspected_stale");

  const auto events = veylqora::state_events_of(db, kFixtures, id);
  REQUIRE(events.size() == 2);  // creation and the one legal transition
  CHECK(events[1].from_state == "active");
  CHECK(events[1].to_state == "suspected_stale");
  CHECK(events[1].reason == "code changed");
}

TEST_CASE("archived memories are read-only") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  const std::string id =
      veylqora::create_memory(db, clock, ids, kFixtures, decision("x"));
  veylqora::transition_memory(db, clock, kFixtures, id, 1, "archived", "user",
                              "no longer relevant");
  CHECK_THROWS_AS(veylqora::update_memory(db, clock, kFixtures, id, 2, "y", "a", "r"),
                  veylqora::Error);
}

TEST_CASE("supersession links the old memory to its replacement") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  const std::string old_id =
      veylqora::create_memory(db, clock, ids, kFixtures, decision("old"));
  const std::string new_id =
      veylqora::create_memory(db, clock, ids, kFixtures, decision("new"));
  veylqora::supersede_memory(db, clock, kFixtures, old_id, 1, new_id, "agent-b",
                             "replaced");
  const auto old = veylqora::get_memory(db, kFixtures, old_id);
  CHECK(old->state == "superseded");
  CHECK(old->superseded_by == new_id);
}

TEST_CASE("evidence must name an indexed symbol, or the memory is not created at all") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  veylqora::index_project(db, clock, kFixtures);

  veylqora::NewMemory good = decision("verified");
  good.evidence = {{"auth/tokens.py", "auth.tokens.verify_token"}};
  const std::string id = veylqora::create_memory(db, clock, ids, kFixtures, good);
  const auto ev = veylqora::evidence_of(db, kFixtures, id);
  REQUIRE(ev.size() == 1);
  CHECK(ev[0].symbol_fqn == "auth.tokens.verify_token");
  CHECK_FALSE(ev[0].content_fingerprint.empty());

  veylqora::NewMemory bad = decision("cites nothing real");
  bad.evidence = {{"auth/tokens.py", "auth.tokens.no_such_function"}};
  CHECK_THROWS_AS(veylqora::create_memory(db, clock, ids, kFixtures, bad),
                  veylqora::Error);
  // The rejected memory left no row behind, not even a partial one.
  CHECK(veylqora::list_memories(db, kFixtures).size() == 1);
}

TEST_CASE("memories survive a restart") {
  TempDir tmp;
  const fs::path file = tmp.path() / "v.db";
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  std::string id;
  {
    veylqora::Database db(file);
    id = veylqora::create_memory(db, clock, ids, kFixtures, decision("durable"));
    veylqora::update_memory(db, clock, kFixtures, id, 1, "durable v2", "agent-a", "edit");
  }
  veylqora::Database reopened(file);
  const auto m = veylqora::get_memory(reopened, kFixtures, id);
  REQUIRE(m.has_value());
  CHECK(m->content == "durable v2");
  CHECK(veylqora::revisions_of(reopened, kFixtures, id).size() == 2);
}

TEST_CASE("projects are isolated: another project's memory is not found") {
  TempDir tmp;
  const fs::path other = tmp.path() / "other";
  fs::create_directories(other);
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  const std::string id =
      veylqora::create_memory(db, clock, ids, kFixtures, decision("mine"));
  CHECK_FALSE(veylqora::get_memory(db, other, id).has_value());
  CHECK(veylqora::list_memories(db, other).empty());
  CHECK_THROWS_AS(veylqora::update_memory(db, clock, other, id, 1, "stolen", "x", "y"),
                  veylqora::Error);
}

TEST_CASE("required fields are validated") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  veylqora::NewMemory no_author = decision("x");
  no_author.author = "";
  CHECK_THROWS_AS(veylqora::create_memory(db, clock, ids, kFixtures, no_author),
                  veylqora::Error);
  veylqora::NewMemory bad_type = decision("x");
  bad_type.type = "gossip";
  CHECK_THROWS_AS(veylqora::create_memory(db, clock, ids, kFixtures, bad_type),
                  veylqora::Error);
  veylqora::NewMemory bad_scope = decision("x");
  bad_scope.scope = "public";
  CHECK_THROWS_AS(veylqora::create_memory(db, clock, ids, kFixtures, bad_scope),
                  veylqora::Error);
}

TEST_CASE("retention purges only archived memories older than the cutoff") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  veylqora::SequentialIds ids;
  const veylqora::FixedClock early(veylqora::Timestamp{} + 1000s);
  const veylqora::FixedClock late(veylqora::Timestamp{} + 9000s);
  const std::string old_archived =
      veylqora::create_memory(db, early, ids, kFixtures, decision("a"));
  const std::string new_archived =
      veylqora::create_memory(db, late, ids, kFixtures, decision("b"));
  const std::string kept =
      veylqora::create_memory(db, early, ids, kFixtures, decision("c"));
  veylqora::transition_memory(db, early, kFixtures, old_archived, 1, "archived", "u",
                              "old");
  veylqora::transition_memory(db, late, kFixtures, new_archived, 1, "archived", "u",
                              "new");

  const std::size_t removed =
      veylqora::purge_archived(db, kFixtures, veylqora::Timestamp{} + 5000s);
  CHECK(removed == 1);
  CHECK_FALSE(veylqora::get_memory(db, kFixtures, old_archived).has_value());
  CHECK(veylqora::get_memory(db, kFixtures, new_archived).has_value());
  CHECK(veylqora::get_memory(db, kFixtures, kept).has_value());
}

TEST_CASE("export, import into an empty database, and export again: byte-identical") {
  TempDir tmp;
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  const fs::path first = tmp.path() / "first.db";
  const fs::path second = tmp.path() / "second.db";
  const fs::path out1 = tmp.path() / "export1.jsonl";
  const fs::path out2 = tmp.path() / "export2.jsonl";

  {
    veylqora::Database db(first);
    veylqora::index_project(db, clock, kFixtures);
    veylqora::NewMemory m =
        decision("line one\nline \"two\"\twith tab \\ and unicode \xC3\xA9");
    m.evidence = {{"auth/tokens.py", "auth.tokens.verify_token"}};
    const std::string id = veylqora::create_memory(db, clock, ids, kFixtures, m);
    veylqora::update_memory(db, clock, kFixtures, id, 1, "revised", "agent-b",
                            "second look");
    const std::string other =
        veylqora::create_memory(db, clock, ids, kFixtures, decision("second"));
    veylqora::supersede_memory(db, clock, kFixtures, id, 2, other, "agent-b", "replaced");
    CHECK(veylqora::export_memories(db, kFixtures, out1) == 2);
  }
  {
    veylqora::Database db(second);
    CHECK(veylqora::import_memories(db, out1) == 2);
    CHECK(veylqora::export_memories(db, kFixtures, out2) == 2);
  }
  CHECK(read_all(out1) == read_all(out2));
}

TEST_CASE("a corrupt import file is rejected as a whole") {
  TempDir tmp;
  const fs::path bad = tmp.path() / "bad.jsonl";
  {
    std::ofstream out(bad, std::ios::binary);
    out << "{\"kind\":\"project\",\"id\":\"p\",\"name\":\"n\",\"root_path\":\"r\","
           "\"created_at\":\"t\"}\n";
    out << "{\"kind\":\"memory\",\"id\":\n";
  }
  veylqora::Database db(tmp.path() / "v.db");
  CHECK_THROWS_AS(veylqora::import_memories(db, bad), veylqora::Error);
  // All or nothing: the project row from the valid first line was rolled back too.
  CHECK(veylqora::list_memories(db, tmp.path()).empty());
}
