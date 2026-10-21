// M3.3 fuzz target: search queries are untrusted input. Any byte string must come
// back as a result or a veylqora::Error (a 4xx at the API), never a crash.

#include "veylqora/api/service.hpp"
#include "veylqora/core/error.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/storage/database.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = VEYLQORA_FIXTURE_DIR "/python";

struct State {
  std::unique_ptr<veylqora::Database> db;
};

// One indexed database for the whole run, so each input exercises retrieval
// rather than indexing.
State& state() {
  static State s = [] {
    State st;
    const fs::path file = fs::temp_directory_path() / "veylqora-fuzz-query.db";
    std::error_code ec;
    fs::remove(file, ec);
    st.db = std::make_unique<veylqora::Database>(file);
    const veylqora::SystemClock clock;
    veylqora::index_project(*st.db, clock, kFixtures);
    return st;
  }();
  return s;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string query(reinterpret_cast<const char*>(data), size);
  try {
    (void)veylqora::api::retrieve(*state().db, kFixtures, query, "hybrid", 10, "", "");
    (void)veylqora::api::retrieve(*state().db, kFixtures, query, "lexical", 10, "", "");
  } catch (const veylqora::Error&) {
    // A rejected query is the documented 4xx path.
  }
  return 0;
}
