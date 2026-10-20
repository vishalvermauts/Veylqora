// M2.6 multi-agent coordination (ADR-0017): concurrent writers with no lost updates,
// visibility by scope, approval, and an auditable history of shared writes.

#include "veylqora/core/error.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/memory/memory.hpp"
#include "veylqora/storage/database.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using veylqora::test::TempDir;

namespace {

const fs::path kRoot = VEYLQORA_FIXTURE_DIR "/python";
constexpr std::size_t kAgents = 40;        // ADR-0017 threshold
constexpr std::size_t kUpdatesEach = 5;    // ADR-0017 threshold

veylqora::NewMemory memory_of(const std::string& author, const std::string& scope) {
  veylqora::NewMemory m;
  m.type = "decision";
  m.content = "initial by " + author;
  m.author = author;
  m.scope = scope;
  return m;
}

}  // namespace

TEST_CASE("40 agents on one shared memory: no lost updates, every write in the history") {
  TempDir tmp;
  const fs::path file = tmp.path() / "v.db";
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  std::string id;
  {
    veylqora::Database db(file);
    id = veylqora::create_memory(db, clock, ids, kRoot, memory_of("seed", "shared"));
  }

  std::vector<std::size_t> successes(kAgents, 0);
  std::vector<std::thread> agents;
  for (std::size_t a = 0; a < kAgents; ++a) {
    agents.emplace_back([&, a] {
      // Each agent has its own connection, as separate processes would.
      veylqora::Database db(file);
      const std::string who = "agent-" + std::to_string(a);
      while (successes[a] < kUpdatesEach) {
        const auto current = veylqora::get_memory(db, kRoot, id, who);
        try {
          veylqora::update_memory(db, clock, kRoot, id, current->revision_no,
                                  who + " update " + std::to_string(successes[a]), who, "coordinate");
          ++successes[a];
        } catch (const veylqora::ConflictError&) {
          // Another agent wrote first; read again and retry.
        }
      }
    });
  }
  for (auto& t : agents) t.join();

  veylqora::Database db(file);
  const auto final_state = veylqora::get_memory(db, kRoot, id, "");
  const auto total = static_cast<std::int64_t>(kAgents * kUpdatesEach);
  CHECK(final_state->revision_no == 1 + total);  // every success advanced the revision exactly once

  const auto history = veylqora::revisions_of(db, kRoot, id);
  CHECK(history.size() == static_cast<std::size_t>(1 + total));
  std::map<std::string, std::size_t> by_actor;
  for (const auto& r : history) ++by_actor[r.actor];
  for (std::size_t a = 0; a < kAgents; ++a) {
    CHECK(by_actor["agent-" + std::to_string(a)] == kUpdatesEach);
  }
  // Revisions are contiguous: nothing was skipped or written twice.
  for (std::size_t i = 0; i < history.size(); ++i) {
    CHECK(history[i].revision_no == static_cast<std::int64_t>(i + 1));
  }
}

TEST_CASE("a private memory is invisible to other agents, and they cannot change it") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  const std::string id = veylqora::create_memory(db, clock, ids, kRoot, memory_of("alice", "private"));

  CHECK(veylqora::get_memory(db, kRoot, id, "alice").has_value());
  CHECK_FALSE(veylqora::get_memory(db, kRoot, id, "bob").has_value());
  CHECK(veylqora::list_memories(db, kRoot, "", "bob").empty());
  CHECK(veylqora::list_memories(db, kRoot, "", "alice").size() == 1);
  CHECK_THROWS_AS(veylqora::update_memory(db, clock, kRoot, id, 1, "stolen", "bob", "x"),
                  veylqora::NotFoundError);
  CHECK(veylqora::get_memory(db, kRoot, id, "alice")->content == "initial by alice");
}

TEST_CASE("an approved memory is hidden until approved, and approval is cleared by a change") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  const std::string id = veylqora::create_memory(db, clock, ids, kRoot, memory_of("alice", "approved"));

  CHECK_FALSE(veylqora::get_memory(db, kRoot, id, "bob").has_value());
  CHECK_THROWS_AS(veylqora::approve_memory(db, clock, kRoot, id, 1, "alice", "self"), veylqora::Error);
  veylqora::approve_memory(db, clock, kRoot, id, 1, "carol", "reviewed the claim");

  const auto seen = veylqora::get_memory(db, kRoot, id, "bob");
  REQUIRE(seen.has_value());
  CHECK(seen->approved_by == "carol");

  veylqora::update_memory(db, clock, kRoot, id, 2, "a changed claim", "alice", "revised");
  CHECK_FALSE(veylqora::get_memory(db, kRoot, id, "bob").has_value());  // the approval was cleared
}

TEST_CASE("an interrupted write leaves no partial memory behind") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  veylqora::NewMemory m = memory_of("alice", "shared");
  m.evidence = {{"auth.py", "auth.no_such_symbol"}};  // fails after the memory row is written
  CHECK_THROWS_AS(veylqora::create_memory(db, clock, ids, kRoot, m), veylqora::Error);
  CHECK(veylqora::list_memories(db, kRoot, "", "").empty());
}
