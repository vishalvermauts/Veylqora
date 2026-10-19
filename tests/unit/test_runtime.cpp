#include "veylqora/core/runtime.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

TEST_CASE("timestamps format as UTC ISO-8601 to the second") {
  CHECK(veylqora::to_iso8601(veylqora::Timestamp{}) == "1970-01-01T00:00:00Z");
  const veylqora::Timestamp t = veylqora::Timestamp{} + std::chrono::seconds(1700000000);
  CHECK(veylqora::to_iso8601(t) == "2023-11-14T22:13:20Z");
}

TEST_CASE("fixed clock returns exactly the instant it was given") {
  const veylqora::Timestamp t = veylqora::Timestamp{} + 42s;
  const veylqora::FixedClock clock(t);
  CHECK(clock.now() == t);
  CHECK(clock.now() == t);
}
