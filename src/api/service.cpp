#include "veylqora/api/service.hpp"

#include "veylqora/core/project.hpp"
#include "veylqora/graph/query.hpp"
#include "veylqora/observability/observability.hpp"
#include "veylqora/retrieval/search.hpp"

namespace veylqora::api {
namespace fs = std::filesystem;
namespace {

constexpr const char* kVersion = VEYLQORA_VERSION;

// Required fields are checked here so a malformed request is "invalid" (400),
// not an internal error.
std::string text_field(const Json& body, const char* key, bool required = true) {
  if (!body.is_object() || !body.contains(key)) {
    if (required) throw Error(std::string("missing field: ") + key);
    return {};
  }
  if (!body.at(key).is_string())
    throw Error(std::string("field must be a string: ") + key);
  return body.at(key).get<std::string>();
}

std::int64_t int_field(const Json& body, const char* key) {
  if (!body.is_object() || !body.contains(key) || !body.at(key).is_number_integer()) {
    throw Error(std::string("field must be an integer: ") + key);
  }
  return body.at(key).get<std::int64_t>();
}

Json memory_json(const Memory& m) {
  Json j = {{"id", m.id},
            {"type", m.type},
            {"content", m.content},
            {"state", m.state},
            {"scope", m.scope},
            {"author", m.author},
            {"revision", m.revision_no},
            {"created_at", m.created_at},
            {"approved_by", m.approved_by},
            {"updated_at", m.updated_at}};
  if (!m.superseded_by.empty()) j["superseded_by"] = m.superseded_by;
  return j;
}

std::int64_t count(Database& db, const char* sql, const std::string& pid) {
  Statement q(db, sql);
  q.bind(1, pid);
  q.step();
  return q.integer(0);
}

}  // namespace

Json health() { return Json{{"ok", true}, {"version", kVersion}}; }

Json status(Database& db, const fs::path& root) {
  const std::string pid = project_id_for(root);
  Json out = {{"project_root", root.generic_string()},
              {"indexed", false},
              {"files", 0},
              {"symbols", 0},
              {"memories", 0}};
  Statement p(db, "SELECT name FROM project WHERE id = ?");
  p.bind(1, pid);
  if (p.step()) {
    out["indexed"] = true;
    out["project"] = p.text(0);
  }
  out["files"] = count(db, "SELECT COUNT(*) FROM file WHERE project_id = ?", pid);
  out["symbols"] = count(db, "SELECT COUNT(*) FROM symbol WHERE project_id = ?", pid);
  out["memories"] = count(db, "SELECT COUNT(*) FROM memory WHERE project_id = ?", pid);
  return out;
}

Json index(Database& db, const Clock& clock, const fs::path& root,
           const IndexOptions& options) {
  const IndexStats s = index_project(db, clock, root, options);
  audit(db, clock, "indexer", "index.run", "project", project_id_for(root));
  purge_expired(db, clock, ObservabilityOptions{});
  Json out = {
      {"indexed", s.files_indexed},           {"unchanged", s.files_unchanged},
      {"removed", s.files_removed},           {"skipped", s.files_skipped},
      {"unreadable", s.files_unreadable},     {"partial", s.files_with_syntax_errors},
      {"symbols_written", s.symbols_written}, {"canceled", s.canceled}};
  // A completed run that changed something may put memories in doubt (ADR-0014).
  if (!s.canceled && s.files_indexed + s.files_removed + s.files_skipped > 0) {
    const RevalidationStats r = revalidate_project(db, clock, root);
    out["memories_checked"] = r.checked;
    out["memories_flagged"] = r.flagged;
    out["memories_restored"] = r.restored;
    out["evidence_reanchored"] = r.reanchored;
  }
  return out;
}

Json search(Database& db, const fs::path& root, std::string_view name) {
  Json hits = Json::array();
  for (const SymbolHit& h : find_symbols(db, root, name)) {
    hits.push_back(
        {{"path", h.path}, {"kind", h.kind}, {"fqn", h.fqn}, {"line", h.start_line}});
  }
  return Json{{"name", std::string(name)}, {"hits", hits}};
}

Json graph(Database& db, const fs::path& root, std::string_view name) {
  Json dependents = Json::array();
  for (const GraphEdge& e : dependents_of(db, root, name)) {
    dependents.push_back({{"kind", e.kind},
                          {"from", e.src_fqn},
                          {"path", e.src_path},
                          {"line", e.line},
                          {"resolution", e.resolution}});
  }
  Json dependencies = Json::array();
  for (const GraphEdge& e : dependencies_of(db, root, name)) {
    dependencies.push_back({{"kind", e.kind},
                            {"to", e.dst_fqn.empty() ? Json(nullptr) : Json(e.dst_fqn)},
                            {"hint", e.dst_hint},
                            {"resolution", e.resolution},
                            {"line", e.line}});
  }
  return Json{{"name", std::string(name)},
              {"dependents", dependents},
              {"dependencies", dependencies}};
}

Json memory_create(Database& db, const Clock& clock, IdGenerator& ids,
                   const fs::path& root, const Json& body) {
  NewMemory m;
  m.type = text_field(body, "type");
  m.content = text_field(body, "content");
  m.author = text_field(body, "author");
  const std::string scope = text_field(body, "scope", false);
  if (!scope.empty()) m.scope = scope;
  if (body.contains("evidence")) {
    if (!body.at("evidence").is_array()) throw Error("field must be an array: evidence");
    for (const Json& e : body.at("evidence")) {
      m.evidence.push_back(
          EvidenceRef{text_field(e, "rel_path"), text_field(e, "symbol_fqn")});
    }
  }
  const std::string id = create_memory(db, clock, ids, root, m);
  audit(db, clock, m.author, "memory.create", "memory", id);
  return memory_json(*get_memory(db, root, id));
}

Json memory_list(Database& db, const fs::path& root, std::string_view state,
                 std::string_view viewer) {
  Json out = Json::array();
  for (const Memory& m : veylqora::list_memories(db, root, state, viewer))
    out.push_back(memory_json(m));
  return out;
}

Json memory_approve(Database& db, const Clock& clock, const fs::path& root,
                    std::string_view id, const Json& body) {
  const std::string approver = text_field(body, "approver");
  approve_memory(db, clock, root, id, int_field(body, "expected_revision"), approver,
                 text_field(body, "reason"));
  audit(db, clock, approver, "memory.approve", "memory", id);
  return memory_json(*get_memory(db, root, id));
}

Json memory_inspect(Database& db, const fs::path& root, std::string_view id,
                    std::string_view viewer) {
  const auto m = get_memory(db, root, id, viewer);
  if (!m) throw NotFoundError("no memory with id " + std::string(id));
  Json evidence = Json::array();
  for (const EvidenceRow& e : evidence_of(db, root, id)) {
    evidence.push_back({{"path", e.rel_path},
                        {"symbol", e.symbol_fqn},
                        {"fingerprint", e.content_fingerprint}});
  }
  Json history = Json::array();
  for (const Revision& r : revisions_of(db, root, id)) {
    history.push_back({{"revision", r.revision_no},
                       {"content", r.content},
                       {"state", r.state},
                       {"actor", r.actor},
                       {"reason", r.reason},
                       {"at", r.changed_at}});
  }
  Json transitions = Json::array();
  for (const StateEvent& e : state_events_of(db, root, id)) {
    transitions.push_back({{"from", e.from_state},
                           {"to", e.to_state},
                           {"actor", e.actor},
                           {"reason", e.reason},
                           {"at", e.at}});
  }
  Json out = memory_json(*m);
  out["evidence"] = evidence;
  out["history"] = history;
  out["transitions"] = transitions;
  return out;
}

Json memory_update(Database& db, const Clock& clock, const fs::path& root,
                   std::string_view id, const Json& body) {
  const std::string actor = text_field(body, "actor");
  update_memory(db, clock, root, id, int_field(body, "expected_revision"),
                text_field(body, "content"), actor, text_field(body, "reason"));
  audit(db, clock, actor, "memory.update", "memory", id);
  return memory_json(*get_memory(db, root, id));
}

Json retrieve(Database& db, const fs::path& root, std::string_view query,
              std::string_view mode, std::size_t top_k, std::string_view kind,
              std::string_view path_prefix) {
  RetrievalOptions options;
  if (mode == "hybrid") {
    options.mode = Mode::hybrid;
  } else if (mode == "lexical") {
    options.mode = Mode::lexical;
  } else if (mode == "vector") {
    options.mode = Mode::vector;
  } else {
    throw Error("mode must be hybrid, lexical or vector");
  }
  options.top_k = top_k;
  options.kind = std::string(kind);
  options.path_prefix = std::string(path_prefix);
  Json hits = Json::array();
  for (const Hit& h : veylqora::retrieve(db, root, query, options)) {
    hits.push_back({{"fqn", h.fqn},
                    {"path", h.path},
                    {"kind", h.kind},
                    {"line", h.line},
                    {"score", h.score}});
  }
  return Json{{"query", std::string(query)}, {"hits", hits}};
}

Json memory_accept(Database& db, const Clock& clock, const fs::path& root,
                   std::string_view id, const Json& body) {
  const std::string actor = text_field(body, "actor");
  accept_memory(db, clock, root, id, int_field(body, "expected_revision"), actor,
                text_field(body, "reason"));
  audit(db, clock, actor, "memory.accept", "memory", id);
  return memory_json(*get_memory(db, root, id));
}

Json memory_transition(Database& db, const Clock& clock, const fs::path& root,
                       std::string_view id, const Json& body) {
  const std::string actor = text_field(body, "actor");
  transition_memory(db, clock, root, id, int_field(body, "expected_revision"),
                    text_field(body, "to_state"), actor, text_field(body, "reason"));
  audit(db, clock, actor, "memory.transition", "memory", id);
  return memory_json(*get_memory(db, root, id));
}

Json metrics(Database& db, const fs::path& root) {
  const std::string pid = project_id_for(root);
  Json memories = Json::object();
  {
    Statement q(db, "SELECT state, COUNT(*) FROM memory WHERE project_id = ? GROUP BY state");
    q.bind(1, pid);
    while (q.step()) memories[q.text(0)] = q.integer(1);
  }
  Json actions = Json::object();
  {
    Statement q(db, "SELECT action, COUNT(*) FROM audit_log GROUP BY action");
    while (q.step()) actions[q.text(0)] = q.integer(1);
  }
  return Json{
      {"project_root", root.generic_string()},
      {"files", count(db, "SELECT COUNT(*) FROM file WHERE project_id = ?", pid)},
      {"symbols", count(db, "SELECT COUNT(*) FROM symbol WHERE project_id = ?", pid)},
      {"memories_by_state", memories},
      {"queries_logged", count(db, "SELECT COUNT(*) FROM query_log WHERE project_id = ?", pid)},
      // The audit log has no project column, so its counts are instance-wide.
      {"audit_events_by_action", actions}};
}

Json doctor(Database& db) {
  std::string integrity;
  {
    Statement q(db, "PRAGMA integrity_check");
    q.step();
    integrity = q.text(0);
  }
  std::int64_t violations = 0;
  {
    Statement q(db, "PRAGMA foreign_key_check");
    while (q.step()) ++violations;
  }
  std::int64_t version = 0;
  {
    Statement q(db, "SELECT COALESCE(MAX(version), 0) FROM schema_version");
    q.step();
    version = q.integer(0);
  }
  return Json{{"ok", integrity == "ok" && violations == 0},
              {"schema_version", version},
              {"integrity", integrity},
              {"foreign_key_violations", violations}};
}

}  // namespace veylqora::api
