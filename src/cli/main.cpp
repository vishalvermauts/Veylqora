#include "veylqora/api/service.hpp"
#include "veylqora/core/error.hpp"
#include "veylqora/core/file_lock.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/graph/query.hpp"
#include "veylqora/memory/transfer.hpp"
#include "veylqora/observability/observability.hpp"
#include "veylqora/server/server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <httplib.h>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace api = veylqora::api;
using api::Json;
namespace fs = std::filesystem;

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNotFound = 3;
constexpr int kExitConflict = 4;

std::atomic<bool> g_interrupted{false};
extern "C" void on_interrupt(int /*signal*/) { g_interrupted.store(true); }

void usage() {
  std::cerr
      << "usage:\n"
         "  veylqora --version\n"
         "  veylqora init --db <file>\n"
         "  veylqora index <root> --db <file> | --server <url>\n"
         "  veylqora search <name> --root <root> --db <file> | --server <url>\n"
         "  veylqora graph <name> --root <root> --db <file> | --server <url>\n"
         "  veylqora status --root <root> --db <file> | --server <url>\n"
         "  veylqora doctor --db <file> | --server <url>\n"
         "  veylqora memory add --root <root> --type <t> --content <c> --author <a>\n"
         "                      [--scope <s>] [--evidence <path>:<fqn>]...  (+ "
         "db/server)\n"
         "  veylqora memory list --root <root> [--state <s>]               (+ "
         "db/server)\n"
         "  veylqora memory inspect <id> --root <root>                     (+ "
         "db/server)\n"
         "  veylqora memory update <id> --root <root> --expected <n> --content <c>\n"
         "                      --actor <a> --reason <r>                   (+ "
         "db/server)\n"
         "  veylqora memory accept <id> --root <root> --expected <n> --actor <a> "
         "--reason <r>\n"
         "  veylqora memory transition <id> --root <root> --expected <n> --to <s>\n"
         "                      --actor <a> --reason <r>                   (+ "
         "db/server)\n"
         "  veylqora memory export <file> --root <root> --db <file>\n"
         "  veylqora memory import <file> --db <file>\n"
         "  veylqora serve --db <file> [--host 127.0.0.1] [--port 8765] "
         "[--query-log-every N]\n"
         "global: --json prints machine-readable output\n"
         "exit codes: 0 ok, 1 error, 2 usage, 3 not found, 4 conflict\n";
}

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::vector<std::string>> flags;
  bool json = false;

  std::string flag(const std::string& name) const {
    const auto it = flags.find(name);
    return it == flags.end() || it->second.empty() ? std::string{} : it->second.back();
  }
  std::vector<std::string> all(const std::string& name) const {
    const auto it = flags.find(name);
    return it == flags.end() ? std::vector<std::string>{} : it->second;
  }
};

// Returns false on a malformed command line (a flag without its value).
bool parse_args(int argc, char** argv, Args& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--json") {
      out.json = true;
    } else if (a.rfind("--", 0) == 0) {
      if (i + 1 >= argc) return false;
      out.flags[a.substr(2)].push_back(argv[++i]);
    } else {
      out.positional.push_back(a);
    }
  }
  return true;
}

// A backend answers each operation with the same JSON whether it reads the
// database file directly or talks to a running server.
class Backend {
 public:
  virtual ~Backend() = default;
  virtual Json status(const fs::path& root) = 0;
  virtual Json index(const fs::path& root) = 0;
  virtual Json search(const fs::path& root, const std::string& name) = 0;
  virtual Json graph(const fs::path& root, const std::string& name) = 0;
  virtual Json doctor() = 0;
  virtual Json memory_create(const Json& body) = 0;
  virtual Json memory_list(const fs::path& root, const std::string& state) = 0;
  virtual Json memory_inspect(const fs::path& root, const std::string& id) = 0;
  virtual Json memory_update(const std::string& id, const Json& body) = 0;
  virtual Json memory_accept(const std::string& id, const Json& body) = 0;
  virtual Json memory_transition(const std::string& id, const Json& body) = 0;
};

class DirectBackend final : public Backend {
 public:
  explicit DirectBackend(veylqora::Database& db) : db_(db) {}
  Json status(const fs::path& root) override { return api::status(db_, root); }
  Json index(const fs::path& root) override { return api::index(db_, clock_, root); }
  Json search(const fs::path& root, const std::string& name) override {
    return api::search(db_, root, name);
  }
  Json graph(const fs::path& root, const std::string& name) override {
    return api::graph(db_, root, name);
  }
  Json doctor() override { return api::doctor(db_); }
  Json memory_create(const Json& body) override {
    return api::memory_create(db_, clock_, ids_,
                              fs::path(body.at("root").get<std::string>()), body);
  }
  Json memory_list(const fs::path& root, const std::string& state) override {
    return api::memory_list(db_, root, state);
  }
  Json memory_inspect(const fs::path& root, const std::string& id) override {
    return api::memory_inspect(db_, root, id);
  }
  Json memory_update(const std::string& id, const Json& body) override {
    return api::memory_update(db_, clock_, fs::path(body.at("root").get<std::string>()),
                              id, body);
  }
  Json memory_accept(const std::string& id, const Json& body) override {
    return api::memory_accept(db_, clock_, fs::path(body.at("root").get<std::string>()),
                              id, body);
  }
  Json memory_transition(const std::string& id, const Json& body) override {
    return api::memory_transition(db_, clock_,
                                  fs::path(body.at("root").get<std::string>()), id, body);
  }

 private:
  veylqora::Database& db_;
  veylqora::SystemClock clock_;
  veylqora::RandomIds ids_;
};

// Maps an HTTP failure back to the exception type the direct backend throws,
// so both backends share the same exit codes.
[[noreturn]] void throw_for(const httplib::Result& res) {
  if (!res) throw veylqora::Error("cannot reach the server");
  std::string message = "request failed with status " + std::to_string(res->status);
  try {
    message = Json::parse(res->body).at("error").at("message").get<std::string>();
  } catch (const std::exception&) {
    // keep the status-based message
  }
  if (res->status == 404) throw veylqora::NotFoundError(message);
  if (res->status == 409) throw veylqora::ConflictError(message);
  throw veylqora::Error(message);
}

class RemoteBackend final : public Backend {
 public:
  explicit RemoteBackend(const std::string& url) : client_(url) {
    client_.set_connection_timeout(5);
  }

  Json status(const fs::path& root) override {
    return get("/api/v1/status", {{"root", root.string()}});
  }
  Json index(const fs::path& root) override {
    return post("/api/v1/index", Json{{"root", root.string()}});
  }
  Json search(const fs::path& root, const std::string& name) override {
    return get("/api/v1/search", {{"root", root.string()}, {"name", name}});
  }
  Json graph(const fs::path& root, const std::string& name) override {
    return get("/api/v1/graph", {{"root", root.string()}, {"name", name}});
  }
  Json doctor() override { return get("/api/v1/doctor", {}); }
  Json memory_create(const Json& body) override { return post("/api/v1/memories", body); }
  Json memory_list(const fs::path& root, const std::string& state) override {
    httplib::Params p{{"root", root.string()}};
    if (!state.empty()) p.emplace("state", state);
    return get("/api/v1/memories", p);
  }
  Json memory_inspect(const fs::path& root, const std::string& id) override {
    return get("/api/v1/memories/" + id, {{"root", root.string()}});
  }
  Json memory_update(const std::string& id, const Json& body) override {
    return post("/api/v1/memories/" + id + "/update", body);
  }
  Json memory_accept(const std::string& id, const Json& body) override {
    return post("/api/v1/memories/" + id + "/accept", body);
  }
  Json memory_transition(const std::string& id, const Json& body) override {
    return post("/api/v1/memories/" + id + "/transition", body);
  }

 private:
  Json get(const std::string& path, const httplib::Params& params) {
    const httplib::Result res = client_.Get(path, params, httplib::Headers{});
    if (!res || res->status >= 300) throw_for(res);
    return Json::parse(res->body);
  }
  Json post(const std::string& path, const Json& body) {
    const httplib::Result res = client_.Post(path, body.dump(), "application/json");
    if (!res || res->status >= 300) throw_for(res);
    return Json::parse(res->body);
  }
  httplib::Client client_;
};

std::string short_id(const std::string& id) { return id.substr(0, 8); }

void print_human(const std::string& command, const std::string& sub, const Json& j) {
  if (command == "status") {
    std::cout << "project: " << j.value("project", std::string("(not indexed)"))
              << "  indexed: " << (j.value("indexed", false) ? "yes" : "no")
              << "  files: " << j.value("files", 0)
              << "  symbols: " << j.value("symbols", 0)
              << "  memories: " << j.value("memories", 0) << '\n';
  } else if (command == "index") {
    std::cout << "indexed " << j.value("indexed", 0) << ", unchanged "
              << j.value("unchanged", 0) << ", removed " << j.value("removed", 0)
              << ", skipped " << j.value("skipped", 0) << ", unreadable "
              << j.value("unreadable", 0) << ", partial " << j.value("partial", 0)
              << ", symbols " << j.value("symbols_written", 0) << '\n';
  } else if (command == "search") {
    for (const Json& h : j.at("hits")) {
      std::cout << h.at("path").get<std::string>() << ':'
                << h.at("line").get<std::int64_t>() << "  "
                << h.at("kind").get<std::string>() << "  "
                << h.at("fqn").get<std::string>() << '\n';
    }
  } else if (command == "graph") {
    std::cout << "dependents of " << j.at("name").get<std::string>() << ":\n";
    for (const Json& e : j.at("dependents")) {
      std::cout << "  " << e.at("kind").get<std::string>() << "  "
                << e.at("from").get<std::string>() << "  ["
                << e.at("resolution").get<std::string>() << "]  "
                << e.at("path").get<std::string>() << ':'
                << e.at("line").get<std::int64_t>() << '\n';
    }
    std::cout << "dependencies of " << j.at("name").get<std::string>() << ":\n";
    for (const Json& e : j.at("dependencies")) {
      const std::string to = e.at("to").is_null() ? "?" + e.at("hint").get<std::string>()
                                                  : e.at("to").get<std::string>();
      std::cout << "  " << e.at("kind").get<std::string>() << "  " << to << "  ["
                << e.at("resolution").get<std::string>() << "]\n";
    }
  } else if (command == "doctor") {
    std::cout << (j.value("ok", false) ? "ok" : "problems found")
              << "  schema: " << j.value("schema_version", 0)
              << "  integrity: " << j.value("integrity", std::string{})
              << "  foreign key violations: " << j.value("foreign_key_violations", 0)
              << '\n';
  } else if (command == "memory" && (sub == "list")) {
    for (const Json& m : j) {
      std::cout << short_id(m.at("id").get<std::string>()) << "  "
                << m.at("state").get<std::string>() << "  r"
                << m.at("revision").get<std::int64_t>() << "  "
                << m.at("type").get<std::string>() << "  "
                << m.at("content").get<std::string>() << '\n';
    }
  } else if (command == "memory" && sub == "inspect") {
    std::cout << j.at("id").get<std::string>() << "  " << j.at("state").get<std::string>()
              << "  revision " << j.at("revision").get<std::int64_t>() << "  by "
              << j.at("author").get<std::string>() << '\n'
              << "  " << j.at("content").get<std::string>() << '\n';
    for (const Json& e : j.at("evidence")) {
      std::cout << "  evidence: " << e.at("path").get<std::string>() << ':'
                << e.at("symbol").get<std::string>() << '\n';
    }
    for (const Json& h : j.at("history")) {
      std::cout << "  r" << h.at("revision").get<std::int64_t>() << "  "
                << h.at("state").get<std::string>() << "  "
                << h.at("actor").get<std::string>() << "  "
                << h.at("reason").get<std::string>() << '\n';
    }
  } else {  // create, update, transition
    std::cout << j.at("id").get<std::string>() << "  " << j.at("state").get<std::string>()
              << "  revision " << j.at("revision").get<std::int64_t>() << '\n';
  }
}

int exit_for(const std::exception& e) {
  if (dynamic_cast<const veylqora::ConflictError*>(&e) != nullptr) return kExitConflict;
  if (dynamic_cast<const veylqora::NotFoundError*>(&e) != nullptr) return kExitNotFound;
  return kExitError;
}

int run_server(const Args& args, veylqora::Database& db) {
  const std::string host = args.flag("host").empty() ? "127.0.0.1" : args.flag("host");
  const int port = args.flag("port").empty() ? 8765 : std::stoi(args.flag("port"));
  const std::string db_path = args.flag("db");
  veylqora::FileLock lock(db_path + ".lock");  // the daemon is the only writer (ADR-0004)

  veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  veylqora::Server server(db, clock, ids);
  veylqora::ObservabilityOptions obs;
  if (!args.flag("query-log-every").empty())
    obs.query_log_every = std::stoll(args.flag("query-log-every"));
  server.set_observability(obs);
  const int bound = server.bind(host, port);
  if (!args.flag("web").empty()) server.mount_web(args.flag("web"));
  std::cout << "listening on http://" << host << ':' << bound << " (ctrl-c to stop)"
            << std::endl;

  std::signal(SIGINT, on_interrupt);
  std::thread watcher([&] {
    while (!g_interrupted.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop();
  });
  server.serve();
  g_interrupted.store(true);
  watcher.join();
  return kExitOk;
}

int run(const Args& args) {
  if (args.positional.empty()) {
    usage();
    return kExitUsage;
  }
  const std::string command = args.positional[0];
  const std::string sub = args.positional.size() > 1 ? args.positional[1] : std::string{};
  const std::string db_path = args.flag("db");
  const std::string server_url = args.flag("server");
  const fs::path root = args.flag("root");

  if (command == "init") {
    if (db_path.empty()) return usage(), kExitUsage;
    veylqora::FileLock lock(db_path + ".lock");
    veylqora::Database db(db_path);
    std::cout << "initialized " << db_path << '\n';
    return kExitOk;
  }

  if (command == "memory" && (sub == "export" || sub == "import")) {
    if (db_path.empty() || args.positional.size() != 3) return usage(), kExitUsage;
    veylqora::FileLock lock(db_path + ".lock");
    veylqora::Database db(db_path);
    if (sub == "export") {
      if (root.empty()) return usage(), kExitUsage;
      const std::size_t n = veylqora::export_memories(db, root, args.positional[2]);
      std::cout << "exported " << n << " memories to " << args.positional[2] << '\n';
    } else {
      const std::size_t n = veylqora::import_memories(db, args.positional[2]);
      std::cout << "imported " << n << " memories from " << args.positional[2] << '\n';
    }
    return kExitOk;
  }

  if (command == "serve") {
    if (db_path.empty()) return usage(), kExitUsage;
    veylqora::Database db(db_path);
    return run_server(args, db);
  }

  // Everything else runs against a database file or a running server.
  if (server_url.empty() && db_path.empty()) return usage(), kExitUsage;
  std::optional<veylqora::FileLock> lock;
  std::optional<veylqora::Database> db;
  std::optional<DirectBackend> direct;
  std::optional<RemoteBackend> remote;
  Backend* backend = nullptr;
  if (!server_url.empty()) {
    remote.emplace(server_url);
    backend = &*remote;
  } else {
    // Direct access takes the same lock as the daemon, so the two cannot write
    // the file at once (ADR-0004).
    lock.emplace(db_path + ".lock");
    db.emplace(db_path);
    direct.emplace(*db);
    backend = &*direct;
  }

  Json result;
  if (command == "index" && args.positional.size() == 2) {
    result = backend->index(args.positional[1]);
  } else if (command == "search" && args.positional.size() == 2 && !root.empty()) {
    result = backend->search(root, args.positional[1]);
  } else if (command == "graph" && args.positional.size() == 2 && !root.empty()) {
    result = backend->graph(root, args.positional[1]);
  } else if (command == "status" && !root.empty()) {
    result = backend->status(root);
  } else if (command == "doctor") {
    result = backend->doctor();
  } else if (command == "memory" && sub == "add" && !root.empty()) {
    Json body = {{"root", root.string()},
                 {"type", args.flag("type")},
                 {"content", args.flag("content")},
                 {"author", args.flag("author")}};
    if (!args.flag("scope").empty()) body["scope"] = args.flag("scope");
    Json evidence = Json::array();
    for (const std::string& e : args.all("evidence")) {
      const std::size_t colon = e.find(':');
      if (colon == std::string::npos)
        throw veylqora::Error("--evidence must be <path>:<fqn>");
      evidence.push_back(
          {{"rel_path", e.substr(0, colon)}, {"symbol_fqn", e.substr(colon + 1)}});
    }
    body["evidence"] = evidence;
    result = backend->memory_create(body);
  } else if (command == "memory" && sub == "list" && !root.empty()) {
    result = backend->memory_list(root, args.flag("state"));
  } else if (command == "memory" && sub == "inspect" && args.positional.size() == 3 &&
             !root.empty()) {
    result = backend->memory_inspect(root, args.positional[2]);
  } else if (command == "memory" && sub == "update" && args.positional.size() == 3 &&
             !root.empty()) {
    const Json body = {{"root", root.string()},
                       {"expected_revision", std::stoll(args.flag("expected"))},
                       {"content", args.flag("content")},
                       {"actor", args.flag("actor")},
                       {"reason", args.flag("reason")}};
    result = backend->memory_update(args.positional[2], body);
  } else if (command == "memory" && sub == "accept" && args.positional.size() == 3 &&
             !root.empty()) {
    const Json body = {{"root", root.string()},
                       {"expected_revision", std::stoll(args.flag("expected"))},
                       {"actor", args.flag("actor")},
                       {"reason", args.flag("reason")}};
    result = backend->memory_accept(args.positional[2], body);
  } else if (command == "memory" && sub == "transition" && args.positional.size() == 3 &&
             !root.empty()) {
    const Json body = {{"root", root.string()},
                       {"expected_revision", std::stoll(args.flag("expected"))},
                       {"to_state", args.flag("to")},
                       {"actor", args.flag("actor")},
                       {"reason", args.flag("reason")}};
    result = backend->memory_transition(args.positional[2], body);
  } else {
    usage();
    return kExitUsage;
  }

  if (args.json) {
    std::cout << result.dump(2) << '\n';
  } else {
    print_human(command, sub, result);
  }
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "veylqora " << VEYLQORA_VERSION << '\n';
    return kExitOk;
  }
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return kExitUsage;
  }
  try {
    return run(args);
  } catch (const std::exception& e) {
    if (args.json) {
      std::cerr << Json{{"error", {{"message", e.what()}}}}.dump() << '\n';
    } else {
      std::cerr << "veylqora: error: " << e.what() << '\n';
    }
    return exit_for(e);
  }
}
