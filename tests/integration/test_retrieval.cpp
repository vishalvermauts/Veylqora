// M2.1 hybrid retrieval: labelled queries against the Python fixture, with the
// thresholds fixed in ADR-0015; determinism; bounds; filters; isolation.

#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/retrieval/search.hpp"
#include "veylqora/storage/database.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using veylqora::test::TempDir;

namespace {

constexpr double kMinHybridRecallAt10 = 0.80;  // ADR-0015

const fs::path kFixtures = VEYLQORA_FIXTURE_DIR "/python";

struct Labelled {
  std::string query;
  std::set<std::string> relevant;  // fully qualified names, labelled from the source
};

// Written from reading tests/fixtures/python before any ranking was run.
const std::vector<Labelled> kLabelled = {
    {"token verification", {"auth.tokens.verify_token", "auth.tokens.issue_token", "auth.tokens.TokenStore"}},
    {"password hashing", {"auth.passwords.hash_password", "auth.passwords.check_password", "auth.passwords.check_password_async"}},
    {"format money cents", {"billing.invoice.format_money"}},
    {"tax rate lookup", {"billing.tax.tax_for", "billing.tax.TaxTable.lookup"}},
    {"health status route", {"api.routes.health"}},
    {"log request middleware", {"api.middleware.log_request", "api.middleware.RequestLogger.__call__"}},
    {"slugify text", {"util.text.slugify"}},
    {"measure timing wrapper", {"util.timing.measure"}},
};

std::set<std::string> top_k_fqns(const std::vector<veylqora::Hit>& hits, std::size_t k) {
  std::set<std::string> out;
  for (std::size_t i = 0; i < hits.size() && i < k; ++i) out.insert(hits[i].fqn);
  return out;
}

double mean_recall_at_10(veylqora::Database& db, veylqora::Mode mode) {
  double total = 0.0;
  for (const Labelled& l : kLabelled) {
    veylqora::RetrievalOptions o;
    o.mode = mode;
    o.top_k = 10;
    const auto found = top_k_fqns(veylqora::retrieve(db, kFixtures, l.query, o), 10);
    std::size_t hit = 0;
    for (const std::string& r : l.relevant) hit += found.count(r);
    total += static_cast<double>(hit) / static_cast<double>(l.relevant.size());
  }
  return total / static_cast<double>(kLabelled.size());
}

}  // namespace

TEST_CASE("labelled queries: hybrid recall@10 meets the gate (ADR-0015)") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::index_project(db, clock, kFixtures);

  const double lexical = mean_recall_at_10(db, veylqora::Mode::lexical);
  const double vector = mean_recall_at_10(db, veylqora::Mode::vector);
  const double hybrid = mean_recall_at_10(db, veylqora::Mode::hybrid);
  INFO("mean recall@10 lexical=" << lexical << " vector=" << vector << " hybrid=" << hybrid);
  CHECK(hybrid >= kMinHybridRecallAt10);
}

TEST_CASE("retrieval is deterministic for a fixed index, query and options") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::index_project(db, clock, kFixtures);
  const auto first = veylqora::retrieve(db, kFixtures, "token verification");
  for (int run = 0; run < 20; ++run) {
    const auto again = veylqora::retrieve(db, kFixtures, "token verification");
    REQUIRE(again.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
      CHECK(again[i].fqn == first[i].fqn);
      CHECK(again[i].score == first[i].score);
    }
  }
}

TEST_CASE("empty, punctuation-only and over-long queries are refused") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::index_project(db, clock, kFixtures);
  CHECK_THROWS_AS(veylqora::retrieve(db, kFixtures, ""), veylqora::Error);
  CHECK_THROWS_AS(veylqora::retrieve(db, kFixtures, "?!  ,,"), veylqora::Error);
  CHECK_THROWS_AS(veylqora::retrieve(db, kFixtures, std::string(513, 'a')), veylqora::Error);
  veylqora::RetrievalOptions none;
  none.top_k = 0;
  CHECK_THROWS_AS(veylqora::retrieve(db, kFixtures, "token", none), veylqora::Error);
}

TEST_CASE("top_k is capped at the maximum") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::index_project(db, clock, kFixtures);
  veylqora::RetrievalOptions big;
  big.top_k = 100000;
  big.mode = veylqora::Mode::lexical;
  CHECK(veylqora::retrieve(db, kFixtures, "token", big).size() <= veylqora::kMaxResults);
}

TEST_CASE("metadata filters restrict kind and path") {
  TempDir tmp;
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::index_project(db, clock, kFixtures);

  veylqora::RetrievalOptions methods;
  methods.kind = "method";
  for (const auto& h : veylqora::retrieve(db, kFixtures, "lookup register total", methods)) {
    CHECK(h.kind == "method");
  }
  veylqora::RetrievalOptions billing;
  billing.path_prefix = "billing/";
  billing.mode = veylqora::Mode::lexical;
  const auto hits = veylqora::retrieve(db, kFixtures, "token value", billing);
  for (const auto& h : hits) CHECK(h.path.rfind("billing/", 0) == 0);
}

TEST_CASE("projects sharing a database do not see each other's symbols") {
  TempDir tmp;
  const fs::path a = tmp.path() / "a";
  const fs::path b = tmp.path() / "b";
  fs::create_directories(a);
  fs::create_directories(b);
  fs::copy(kFixtures, a, fs::copy_options::recursive);
  fs::copy(kFixtures, b, fs::copy_options::recursive);
  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::index_project(db, clock, a);
  veylqora::index_project(db, clock, b);

  const auto alone = veylqora::retrieve(db, a, "token verification");
  // A third project whose tree contains a and b must not change a's answers.
  veylqora::index_project(db, clock, tmp.path());
  CHECK(veylqora::retrieve(db, a, "token verification").size() == alone.size());
}
