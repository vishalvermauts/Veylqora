#include "veylqora/graph/query.hpp"

#include "veylqora/core/error.hpp"
#include "veylqora/core/identity.hpp"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace veylqora {
namespace fs = std::filesystem;
namespace {

std::string project_id_of(const fs::path& root) {
  std::error_code ec;
  const fs::path canonical = fs::canonical(root, ec);
  if (ec) throw Error("cannot open " + root.string() + ": " + ec.message());
  return content_fingerprint(canonical.generic_string());
}

GraphEdge read_edge(const Statement& q) {
  return GraphEdge{q.text(0), q.text(1), q.text(2),   q.text(3),
                   q.text(4), q.text(5), q.integer(6)};
}

}  // namespace

std::vector<GraphEdge> dependents_of(Database& db, const fs::path& root,
                                     std::string_view name) {
  Statement q(db,
              "SELECT e.kind, s.fqn, f.rel_path, COALESCE(d.fqn, ''), e.dst_hint, "
              "e.resolution, e.line "
              "FROM edge e "
              "JOIN symbol s ON s.id = e.src_symbol_id "
              "JOIN file f ON f.id = s.file_id "
              "JOIN symbol d ON d.id = e.dst_symbol_id "
              "WHERE e.project_id = ? AND d.name = ? AND e.kind != 'contains' "
              "ORDER BY s.fqn, e.line");
  q.bind(1, project_id_of(root));
  q.bind(2, name);
  std::vector<GraphEdge> out;
  while (q.step()) out.push_back(read_edge(q));
  return out;
}

std::vector<GraphEdge> dependencies_of(Database& db, const fs::path& root,
                                       std::string_view name) {
  Statement q(db,
              "SELECT e.kind, s.fqn, f.rel_path, COALESCE(d.fqn, ''), e.dst_hint, "
              "e.resolution, e.line "
              "FROM edge e "
              "JOIN symbol s ON s.id = e.src_symbol_id "
              "JOIN file f ON f.id = s.file_id "
              "LEFT JOIN symbol d ON d.id = e.dst_symbol_id "
              "WHERE e.project_id = ? AND s.name = ? AND e.kind != 'contains' "
              "ORDER BY e.kind, e.dst_hint, e.line");
  q.bind(1, project_id_of(root));
  q.bind(2, name);
  std::vector<GraphEdge> out;
  while (q.step()) out.push_back(read_edge(q));
  return out;
}

std::vector<Reached> neighbourhood(Database& db, const fs::path& root,
                                   std::string_view name, std::size_t hops) {
  const std::string pid = project_id_of(root);
  struct Node {
    std::string id;
    std::string fqn;
    std::string path;
  };

  std::vector<Node> frontier;
  {
    Statement start(
        db,
        "SELECT s.id, s.fqn, f.rel_path FROM symbol s JOIN file f ON f.id = s.file_id "
        "WHERE s.project_id = ? AND s.name = ?");
    start.bind(1, pid);
    start.bind(2, name);
    while (start.step())
      frontier.push_back(Node{start.text(0), start.text(1), start.text(2)});
  }

  // Both directions, resolved and heuristic edges only: an unresolved edge has
  // no destination to walk to.
  Statement out_edges(db,
                      "SELECT d.id, d.fqn, f.rel_path FROM edge e "
                      "JOIN symbol d ON d.id = e.dst_symbol_id "
                      "JOIN file f ON f.id = d.file_id "
                      "WHERE e.src_symbol_id = ? AND e.resolution != 'unresolved'");
  Statement in_edges(db,
                     "SELECT s.id, s.fqn, f.rel_path FROM edge e "
                     "JOIN symbol s ON s.id = e.src_symbol_id "
                     "JOIN file f ON f.id = s.file_id "
                     "WHERE e.dst_symbol_id = ? AND e.resolution != 'unresolved'");

  std::vector<Reached> result;
  std::unordered_set<std::string> seen;
  for (const Node& n : frontier) {
    seen.insert(n.id);
    result.push_back(Reached{n.fqn, n.path, 0});
  }

  for (std::size_t depth = 1; depth <= hops && !frontier.empty(); ++depth) {
    std::vector<Node> next;
    for (const Node& n : frontier) {
      for (Statement* q : {&out_edges, &in_edges}) {
        q->reset();
        q->bind(1, n.id);
        while (q->step()) {
          const std::string id = q->text(0);
          if (!seen.insert(id).second) continue;
          next.push_back(Node{id, q->text(1), q->text(2)});
          result.push_back(Reached{q->text(1), q->text(2), depth});
        }
      }
    }
    frontier = std::move(next);
  }
  return result;
}

std::vector<Reached> change_impact(Database& db, const fs::path& root,
                                   const std::vector<std::string>& rel_paths) {
  const std::string pid = project_id_of(root);
  std::vector<Reached> out;
  std::set<std::string> seen;

  Statement in_file(
      db,
      "SELECT s.id, s.fqn, f.rel_path FROM symbol s JOIN file f ON f.id = s.file_id "
      "WHERE f.project_id = ? AND f.rel_path = ?");
  Statement dependents(db,
                       "SELECT s.fqn, f.rel_path FROM edge e "
                       "JOIN symbol s ON s.id = e.src_symbol_id "
                       "JOIN file f ON f.id = s.file_id "
                       "WHERE e.dst_symbol_id = ? AND e.kind != 'contains' "
                       "AND e.resolution != 'unresolved'");

  for (const std::string& rel : rel_paths) {
    in_file.reset();
    in_file.bind(1, pid);
    in_file.bind(2, rel);
    std::vector<std::pair<std::string, std::string>> changed;  // (id, fqn)
    while (in_file.step()) {
      changed.emplace_back(in_file.text(0), in_file.text(1));
      if (seen.insert(in_file.text(1) + '\x1f' + in_file.text(2)).second) {
        out.push_back(Reached{in_file.text(1), in_file.text(2), 0});
      }
    }
    for (const auto& [id, fqn] : changed) {
      dependents.reset();
      dependents.bind(1, id);
      while (dependents.step()) {
        if (seen.insert(dependents.text(0) + '\x1f' + dependents.text(1)).second) {
          out.push_back(Reached{dependents.text(0), dependents.text(1), 1});
        }
      }
    }
  }
  return out;
}

}  // namespace veylqora
