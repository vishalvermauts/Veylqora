#include "veylqora/graph/resolver.hpp"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace veylqora {
namespace {

// A name with more candidates than this is too ambiguous to link to every one.
constexpr std::size_t kMaxCandidates = 5;

enum class Level { resolved, heuristic, unresolved };

// What kind of symbol a reference is allowed to point at.
enum class Want { callable, classes, module };

struct Sym {
  std::string id;
  std::string fqn;
  std::string name;
  std::string kind;
  std::string file_id;
  std::int64_t start_line;
};

struct Target {
  std::vector<std::size_t> ids;  // indices into Graph::syms
  Level level;
};

struct Graph {
  std::vector<Sym> syms;
  std::unordered_map<std::string, std::vector<std::size_t>> by_fqn;
  std::unordered_map<std::string, std::vector<std::size_t>> by_name;
  std::unordered_map<std::string, std::size_t>
      module_of_file;                                     // file id -> module symbol
  std::unordered_map<std::string, std::string> bindings;  // file\x1flocal -> target fqn
};

const std::vector<std::size_t>& lookup(
    const std::unordered_map<std::string, std::vector<std::size_t>>& m,
    const std::string& key) {
  static const std::vector<std::size_t> kEmpty;
  const auto it = m.find(key);
  return it == m.end() ? kEmpty : it->second;
}

bool wanted(const Sym& s, Want want) {
  switch (want) {
    case Want::callable:
      return s.kind == "function" || s.kind == "method" || s.kind == "class";
    case Want::classes:
      return s.kind == "class";
    case Want::module:
      return s.kind == "module";
  }
  return false;
}

std::vector<std::size_t> filter(const Graph& g, const std::vector<std::size_t>& ids,
                                Want want, const std::string* file_id = nullptr) {
  std::vector<std::size_t> out;
  for (const std::size_t i : ids) {
    const Sym& s = g.syms[i];
    if (!wanted(s, want)) continue;
    if (file_id != nullptr && s.file_id != *file_id) continue;
    out.push_back(i);
  }
  return out;
}

Target bounded(std::vector<std::size_t> ids, Level level) {
  if (ids.empty()) return {{}, Level::unresolved};
  if (ids.size() > kMaxCandidates) return {{}, Level::unresolved};
  return {std::move(ids), level};
}

// Looks a bare name up the way Python would: an import in this file first,
// then a definition in this file, then anywhere in the project as a guess.
Target resolve_name(const Graph& g, const std::string& file_id, const std::string& name,
                    Want want) {
  const auto bound = g.bindings.find(file_id + '\x1f' + name);
  if (bound != g.bindings.end()) {
    auto ids = filter(g, lookup(g.by_fqn, bound->second), want);
    if (!ids.empty()) return bounded(std::move(ids), Level::resolved);
  }
  auto local = filter(g, lookup(g.by_name, name), want, &file_id);
  if (!local.empty()) return bounded(std::move(local), Level::resolved);
  return bounded(filter(g, lookup(g.by_name, name), want), Level::heuristic);
}

// Resolves `receiver.name` where the receiver may be an imported module
// (`import pkg.util` then `pkg.util.helper()`), or an unknown object.
Target resolve_qualified(const Graph& g, const std::string& file_id,
                         const std::string& qualifier, const std::string& name) {
  const std::string first = qualifier.substr(0, qualifier.find('.'));
  const auto bound = g.bindings.find(file_id + '\x1f' + first);
  if (bound != g.bindings.end()) {
    const std::string target =
        bound->second + qualifier.substr(first.size()) + "." + name;
    auto ids = filter(g, lookup(g.by_fqn, target), Want::callable);
    if (!ids.empty()) return bounded(std::move(ids), Level::resolved);
  }
  return bounded(filter(g, lookup(g.by_name, name), Want::callable), Level::heuristic);
}

class EdgeWriter {
 public:
  EdgeWriter(Database& db, const std::string& pid)
      : pid_(pid),
        insert_(
            db,
            "INSERT INTO edge (project_id, src_symbol_id, dst_symbol_id, dst_hint, kind, "
            "resolution, line) VALUES (?, ?, ?, ?, ?, ?, ?)") {}

  // One edge per candidate. An unresolved reference is one edge with no target.
  void emit(const Graph& g, const std::string& src_id, const Target& t,
            const std::string& hint, std::string_view kind, std::int64_t line) {
    if (t.ids.empty()) return write(src_id, nullptr, hint, kind, Level::unresolved, line);
    for (const std::size_t i : t.ids)
      write(src_id, &g.syms[i].id, hint, kind, t.level, line);
  }

 private:
  void write(const std::string& src_id, const std::string* dst_id,
             const std::string& hint, std::string_view kind, Level level,
             std::int64_t line) {
    insert_.reset();
    insert_.bind(1, pid_);
    insert_.bind(2, src_id);
    if (dst_id != nullptr) {
      insert_.bind(3, *dst_id);
    } else {
      insert_.bind_null(3);
    }
    insert_.bind(4, hint);
    insert_.bind(5, kind);
    insert_.bind(6, level == Level::resolved    ? std::string_view{"resolved"}
                    : level == Level::heuristic ? std::string_view{"heuristic"}
                                                : std::string_view{"unresolved"});
    insert_.bind(7, line);
    insert_.step();
  }

  const std::string& pid_;
  Statement insert_;
};

Graph load(Database& db, const std::string& pid) {
  Graph g;
  {
    Statement q(
        db,
        "SELECT s.id, s.fqn, s.name, s.kind, s.file_id, s.start_line FROM symbol s "
        "WHERE s.project_id = ?");
    q.bind(1, pid);
    while (q.step()) {
      g.syms.push_back(
          Sym{q.text(0), q.text(1), q.text(2), q.text(3), q.text(4), q.integer(5)});
    }
  }
  for (std::size_t i = 0; i < g.syms.size(); ++i) {
    const Sym& s = g.syms[i];
    g.by_fqn[s.fqn].push_back(i);
    g.by_name[s.name].push_back(i);
    if (s.kind == "module") g.module_of_file[s.file_id] = i;
  }
  {
    Statement q(db,
                "SELECT b.file_id, b.local_name, b.target_fqn FROM binding b "
                "JOIN file f ON f.id = b.file_id WHERE f.project_id = ?");
    q.bind(1, pid);
    while (q.step()) g.bindings[q.text(0) + '\x1f' + q.text(1)] = q.text(2);
  }
  return g;
}

void write_containment(const Graph& g, EdgeWriter& out) {
  std::unordered_map<std::string, std::size_t> by_fqn_first;
  for (const auto& [fqn, ids] : g.by_fqn) by_fqn_first[fqn] = ids.front();
  for (std::size_t i = 0; i < g.syms.size(); ++i) {
    const Sym& child = g.syms[i];
    if (child.kind == "module") continue;
    const std::size_t dot = child.fqn.rfind('.');
    if (dot == std::string::npos) continue;
    const auto parent = by_fqn_first.find(child.fqn.substr(0, dot));
    if (parent == by_fqn_first.end()) continue;
    out.emit(g, g.syms[parent->second].id, Target{{i}, Level::resolved}, child.name,
             "contains", child.start_line);
  }
}

}  // namespace

void rebuild_graph(Database& db, const std::string& project_id) {
  Statement clear(db, "DELETE FROM edge WHERE project_id = ?");
  clear.bind(1, project_id);
  clear.step();

  const Graph g = load(db, project_id);
  EdgeWriter out(db, project_id);
  write_containment(g, out);

  Statement facts(
      db,
      "SELECT fa.file_id, fa.src_symbol_id, fa.kind, fa.name, fa.qualifier, fa.scope, "
      "fa.line FROM fact fa JOIN file f ON f.id = fa.file_id WHERE f.project_id = ?");
  facts.bind(1, project_id);
  while (facts.step()) {
    const std::string file_id = facts.text(0);
    const std::string src_id = facts.text(1);
    const std::string kind = facts.text(2);
    const std::string name = facts.text(3);
    const std::string qualifier = facts.text(4);
    const std::string scope = facts.text(5);
    const std::int64_t line = facts.integer(6);

    if (kind == "import") {
      const auto module = g.by_fqn.find(name);
      const Target t =
          module == g.by_fqn.end()
              ? Target{{}, Level::unresolved}
              : bounded(filter(g, module->second, Want::module), Level::resolved);
      out.emit(g, src_id, t, name, "imports", line);
    } else if (kind == "inherits") {
      out.emit(g, src_id, resolve_name(g, file_id, name, Want::classes), name, "inherits",
               line);
    } else {  // call
      Target t{{}, Level::unresolved};
      if (qualifier.empty()) {
        t = resolve_name(g, file_id, name, Want::callable);
      } else if (qualifier == "self" && !scope.empty()) {
        t = bounded(filter(g, lookup(g.by_fqn, scope + "." + name), Want::callable),
                    Level::resolved);
        if (t.ids.empty()) t = resolve_qualified(g, file_id, "", name);
      } else {
        t = resolve_qualified(g, file_id, qualifier, name);
      }
      out.emit(g, src_id, t, name, "calls", line);
    }
  }
}

}  // namespace veylqora
