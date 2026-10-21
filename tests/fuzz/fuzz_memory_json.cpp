// M3.3 fuzz target: request bodies to the memory API are untrusted JSON. Any byte
// string must be a stored result or an error the API reports, never a crash.

#include "veylqora/api/service.hpp"
#include "veylqora/core/error.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/storage/database.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = VEYLQORA_FIXTURE_DIR "/python";

struct State {
  std::unique_ptr<veylqora::Database> db;
  veylqora::SystemClock clock;
  veylqora::RandomIds ids;
};

State& state() {
  static State s = [] {
    State st;
    const fs::path file = fs::temp_directory_path() / "veylqora-fuzz-memory.db";
    std::error_code ec;
    fs::remove(file, ec);
    st.db = std::make_unique<veylqora::Database>(file);
    return st;
  }();
  return s;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  State& st = state();
  try {
    const auto body = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(data), size));
    (void)veylqora::api::memory_create(*st.db, st.clock, st.ids, kFixtures, body);
  } catch (const veylqora::Error&) {
    // A rejected body is the documented 400 path.
  } catch (const nlohmann::json::exception&) {
    // Malformed JSON or a field of the wrong type is also a 400.
  }
  return 0;
}
