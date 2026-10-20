// M2.7 observability (ADR-0018): sampled query log without raw text, audit rows
// without payloads, retention enforced by ageing rows, metrics traced to rows.

#include "veylqora/api/service.hpp"
#include "veylqora/core/identity.hpp"
#include "veylqora/core/project.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/observability/observability.hpp"
#include "veylqora/storage/database.hpp"

#include <chrono>
#include <filesystem>
#include <string>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using veylqora::test::TempDir;
namespace api = veylqora::api;
using api::Json;

namespace {

const fs::path kRoot = VEYLQORA_FIXTURE_DIR "/python";

// Every stored value of a table, concatenated, so a test can search it for text.
std::string dump(veylqora::Database& db, const char* sql) {
  veylqora::Statement q(db, sql);
  std::string out;
  while (q.step()) out += q.text(0) + "\n";
  return out;
}

std::int64_t rows(veylqora::Database& db, const char* table) {
  veylqora::Statement q(db, (std::string("SELECT COUNT(*) FROM ") + table).c_str());
  q.step();
  return q.integer(0);
}

veylqora::Timestamp at(int days) {
  return veylqora::Timestamp{} + std::chrono::hours(24 * 365 * 56) +
         std::chrono::hours(24 * days);
}

}  // namespace

TEST_CASE("the sample decision is deterministic and follows the rate") {
  const std::string zero = std::string(8, '0');
  const std::string one = "00000001" + std::string(56, 'f');
  CHECK(veylqora::query_sampled(zero, 2));
  CHECK_FALSE(veylqora::query_sampled(one, 2));
  CHECK(veylqora::query_sampled(one, 1));
  CHECK_FALSE(veylqora::query_sampled(zero, 0));  // opt-out: nothing is logged

  int sampled = 0;
  for (int i = 0; i < 1000; ++i) {
    if (veylqora::query_sampled(veylqora::content_fingerprint("q" + std::to_string(i)), 4))
      ++sampled;
  }
  CHECK(sampled >= 200);
  CHECK(sampled <= 300);
}

TEST_CASE("the query log stores a hash, never the query text") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::SequentialIds ids;
  api::index(db, clock, kRoot);
  const std::string pid = veylqora::project_id_for(kRoot);
  const std::string secret = "where is the SECRET-TOKEN-QUERY handled";

  veylqora::ObservabilityOptions options;
  veylqora::log_query(db, clock, options, pid, "retrieve", secret, 3, 1.5);
  CHECK(rows(db, "query_log") == 1);
  CHECK(dump(db, "SELECT query_sha256 FROM query_log") ==
        veylqora::content_fingerprint(secret) + "\n");
  CHECK(dump(db, "SELECT method||project_id||at FROM query_log").find("SECRET") ==
        std::string::npos);
}

TEST_CASE("opting out of the query log writes nothing") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  api::index(db, clock, kRoot);
  veylqora::ObservabilityOptions off;
  off.query_log_every = 0;
  veylqora::log_query(db, clock, off, veylqora::project_id_for(kRoot), "search", "x", 0, 0.1);
  CHECK(rows(db, "query_log") == 0);
}

TEST_CASE("audit rows record who did what, never the payload") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  api::index(db, clock, kRoot);

  const Json body = {{"type", "decision"},
                     {"content", "PAYLOAD-MUST-NOT-APPEAR-IN-AUDIT"},
                     {"author", "alice"},
                     {"scope", "shared"}};
  const Json created = api::memory_create(db, clock, ids, kRoot, body);
  const std::string id = created.at("id").get<std::string>();

  CHECK(dump(db, "SELECT action||actor||target_id FROM audit_log WHERE action = "
                 "'memory.create'") == "memory.createalice" + id + "\n");
  const std::string all = dump(db, "SELECT actor||action||target_kind||target_id||at "
                                   "FROM audit_log");
  CHECK(all.find("PAYLOAD") == std::string::npos);
  CHECK(all.find("index.run") != std::string::npos);
}

TEST_CASE("retention deletes rows past their period and keeps the rest") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::FixedClock day0(at(0));
  const veylqora::FixedClock day20(at(20));
  api::index(db, day0, kRoot);  // writes an index.run audit row dated day 0
  const std::string pid = veylqora::project_id_for(kRoot);
  veylqora::ObservabilityOptions options;  // 30-day query log, 365-day audit

  veylqora::log_query(db, day0, options, pid, "retrieve", "old query", 1, 1.0);
  veylqora::log_query(db, day20, options, pid, "retrieve", "newer query", 1, 1.0);
  veylqora::audit(db, day0, "alice", "memory.create", "memory", "m1");
  CHECK(rows(db, "query_log") == 2);
  CHECK(rows(db, "audit_log") == 2);

  // Day 31: the day-0 query is past 30 days; the day-20 query is not. Audit rows
  // are within 365 days, so none are removed.
  const veylqora::FixedClock day31(at(31));
  const auto after31 = veylqora::purge_expired(db, day31, options);
  CHECK(after31.query_rows == 1);
  CHECK(after31.audit_rows == 0);
  CHECK(rows(db, "query_log") == 1);
  CHECK(rows(db, "audit_log") == 2);

  // Day 366: both day-0 audit rows are past 365 days. The day-20 query is past 30.
  const veylqora::FixedClock day366(at(366));
  const auto after366 = veylqora::purge_expired(db, day366, options);
  CHECK(after366.query_rows == 1);
  CHECK(after366.audit_rows == 2);
  CHECK(rows(db, "query_log") == 0);
  CHECK(rows(db, "audit_log") == 0);
}

TEST_CASE("metrics values are counts of rows that exist") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  api::index(db, clock, kRoot);
  const std::string pid = veylqora::project_id_for(kRoot);

  for (const char* author : {"alice", "bob"}) {
    const Json body = {
        {"type", "decision"}, {"content", "a claim"}, {"author", author}, {"scope", "shared"}};
    api::memory_create(db, clock, ids, kRoot, body);
  }
  veylqora::log_query(db, clock, veylqora::ObservabilityOptions{}, pid, "search", "q", 1, 0.5);

  const Json m = api::metrics(db, kRoot);
  CHECK(m.at("memories_by_state").at("active").get<std::int64_t>() == 2);
  CHECK(m.at("memories_by_state").at("active").get<std::int64_t>() ==
        api::status(db, kRoot).at("memories").get<std::int64_t>());
  CHECK(m.at("queries_logged").get<std::int64_t>() == 1);
  CHECK(m.at("audit_events_by_action").at("memory.create").get<std::int64_t>() == 2);
  CHECK(m.at("symbols").get<std::int64_t>() == api::status(db, kRoot).at("symbols").get<std::int64_t>());
}
