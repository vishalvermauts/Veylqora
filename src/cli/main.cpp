#include "veylqora/core/error.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/storage/database.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

void usage() {
  std::cerr << "usage:\n"
               "  veylqora --version\n"
               "  veylqora index <root> --db <file>\n"
               "  veylqora search <name> --root <root> --db <file>\n";
}

// Removes `flag <value>` from args and returns the value, or "" if absent.
std::string take_flag(std::vector<std::string>& args, const std::string& flag) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] != flag) continue;
    std::string value = args[i + 1];
    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
               args.begin() + static_cast<std::ptrdiff_t>(i + 2));
    return value;
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    usage();
    return kExitUsage;
  }
  if (args.front() == "--version") {
    std::cout << "veylqora " << VEYLQORA_VERSION << '\n';
    return kExitOk;
  }

  const std::string command = args.front();
  args.erase(args.begin());
  const std::string db_path = take_flag(args, "--db");
  const std::string root = take_flag(args, "--root");

  try {
    if (db_path.empty()) {
      usage();
      return kExitUsage;
    }
    veylqora::Database db(db_path);
    const veylqora::SystemClock clock;

    if (command == "index" && args.size() == 1) {
      const auto s = veylqora::index_project(db, clock, args[0]);
      std::cout << "indexed " << s.files_indexed << " file(s), unchanged "
                << s.files_unchanged << ", removed " << s.files_removed
                << ", symbols written " << s.symbols_written << '\n';
      return kExitOk;
    }
    if (command == "search" && args.size() == 1 && !root.empty()) {
      for (const veylqora::SymbolHit& h : veylqora::find_symbols(db, root, args[0])) {
        std::cout << h.path << ':' << h.start_line << "  " << h.kind << '  ' << h.fqn
                  << '\n';
      }
      return kExitOk;
    }
    usage();
    return kExitUsage;
  } catch (const std::exception& e) {
    std::cerr << "veylqora: error: " << e.what() << '\n';
    return kExitError;
  }
}
