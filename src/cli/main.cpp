#include "veylqora/core/error.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/graph/query.hpp"
#include "veylqora/index/indexer.hpp"
#include "veylqora/storage/database.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Set by SIGINT. The indexer checks it between files and commits what it has.
std::atomic<bool> g_cancel{false};

extern "C" void on_interrupt(int /*signal*/) { g_cancel.store(true); }

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

void usage() {
  std::cerr << "usage:\n"
               "  veylqora --version\n"
               "  veylqora index <root> --db <file>\n"
               "  veylqora search <name> --root <root> --db <file>\n"
               "  veylqora graph <name> --root <root> --db <file>\n";
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

int run_index(veylqora::Database& db, const veylqora::Clock& clock,
              const std::string& root) {
  veylqora::IndexOptions options;
  options.cancel = &g_cancel;
  options.on_progress = [](std::size_t done, std::size_t total) {
    std::cerr << "\r  " << done << "/" << total << " files" << std::flush;
  };
  std::signal(SIGINT, on_interrupt);

  const veylqora::IndexStats s = veylqora::index_project(db, clock, root, options);
  std::cerr << '\n';
  if (s.canceled) {
    std::cout << "interrupted: re-run the same command to resume\n";
    return kExitError;
  }
  std::cout << "indexed " << s.files_indexed << " file(s), unchanged "
            << s.files_unchanged << ", removed " << s.files_removed << ", skipped "
            << s.files_skipped << ", unreadable " << s.files_unreadable << ", partial "
            << s.files_with_syntax_errors << ", symbols written " << s.symbols_written
            << '\n';
  return kExitOk;
}

int run_search(veylqora::Database& db, const std::string& root, const std::string& name) {
  for (const veylqora::SymbolHit& h : veylqora::find_symbols(db, root, name)) {
    std::cout << h.path << ':' << h.start_line << "  " << h.kind << "  " << h.fqn << '\n';
  }
  return kExitOk;
}

int run_graph(veylqora::Database& db, const std::string& root, const std::string& name) {
  std::cout << "dependents of " << name << ":\n";
  for (const veylqora::GraphEdge& e : veylqora::dependents_of(db, root, name)) {
    std::cout << "  " << e.kind << "  " << e.src_fqn << "  [" << e.resolution << "]  "
              << e.src_path << ':' << e.line << '\n';
  }
  std::cout << "dependencies of " << name << ":\n";
  for (const veylqora::GraphEdge& e : veylqora::dependencies_of(db, root, name)) {
    const std::string target = e.dst_fqn.empty() ? "?" + e.dst_hint : e.dst_fqn;
    std::cout << "  " << e.kind << "  " << target << "  [" << e.resolution << "]\n";
  }
  return kExitOk;
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

    if (command == "index" && args.size() == 1) return run_index(db, clock, args[0]);
    if (command == "search" && args.size() == 1 && !root.empty()) {
      return run_search(db, root, args[0]);
    }
    if (command == "graph" && args.size() == 1 && !root.empty()) {
      return run_graph(db, root, args[0]);
    }
    usage();
    return kExitUsage;
  } catch (const std::exception& e) {
    std::cerr << "veylqora: error: " << e.what() << '\n';
    return kExitError;
  }
}
