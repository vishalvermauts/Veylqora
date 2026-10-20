#include "veylqora/memory/memory.hpp"

#include "veylqora/core/project.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace veylqora {
namespace {

constexpr std::array<std::string_view, 6> kTypes = {"decision",     "observation",
                                                    "task_outcome", "failed_approach",
                                                    "requirement",  "constraint"};
constexpr std::array<std::string_view, 3> kScopes = {"private", "shared", "approved"};
constexpr std::array<std::string_view, 6> kStates = {
    "active",      "suspected_stale",  "superseded",
    "invalidated", "evidence_missing", "archived"};

// The transition table of plan 6.2 and ADR-0011. Archiving is allowed from any
// state and is handled separately. Nothing else is allowed: a change to the code
// can only raise a question, and only a later decision can answer it.
constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kTransitions = {{
    {"active", "suspected_stale"},
    {"active", "evidence_missing"},
    {"active", "superseded"},
    {"suspected_stale", "active"},
    {"suspected_stale", "invalidated"},
    {"suspected_stale", "superseded"},
    {"evidence_missing", "active"},
    {"evidence_missing", "invalidated"},
}};

template <typename Range>
bool contains(const Range& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool transition_allowed(std::string_view from, std::string_view to) {
  if (to == "archived") return true;
  return std::any_of(kTransitions.begin(), kTransitions.end(),
                     [&](const auto& t) { return t.first == from && t.second == to; });
}

void require(bool ok, const char* message) {
  if (!ok) throw Error(message);
}

struct Row {
  std::string id;
  std::string state;
  std::string content;
  std::int64_t revision;
  std::string scope;
  std::string author;
  std::string approved_by;
};

// Who may see a memory (ADR-0017). An empty viewer is the local operator.
bool visible_to(std::string_view scope, std::string_view author, std::string_view approved_by,
                std::string_view viewer) {
  if (viewer.empty() || author == viewer || scope == "shared") return true;
  return scope == "approved" && !approved_by.empty();
}

// A writer must be able to see the memory. Anyone who can see a memory can change
// it, so a private memory (visible only to its author) is changed only by its
// author, and a memory another agent cannot see is reported as not found.
void authorise_write(const Row& row, std::string_view actor) {
  require(!actor.empty(), "an actor is required for attribution");
  if (!visible_to(row.scope, row.author, row.approved_by, actor)) {
    throw NotFoundError("no memory with id " + row.id);
  }
}

std::string project_of(const std::filesystem::path& root) { return project_id_for(root); }

// Ensures the project row exists, so memories do not require a prior index run.
void ensure_project(Database& db, const std::filesystem::path& root,
                    const std::string& pid, const std::string& now) {
  std::error_code ec;
  const std::filesystem::path canonical = std::filesystem::canonical(root, ec);
  Statement p(db,
              "INSERT INTO project (id, name, root_path, created_at) VALUES (?, ?, ?, ?) "
              "ON CONFLICT (id) DO NOTHING");
  p.bind(1, pid);
  p.bind(2, canonical.filename().string());
  p.bind(3, canonical.generic_string());
  p.bind(4, now);
  p.step();
}

// Loads a memory of this project. A memory in another project is "not found",
// so existence is not revealed across projects.
Row load(Database& db, const std::string& pid, std::string_view id) {
  Statement q(db,
              "SELECT id, state, content, revision_no, scope, author, approved_by FROM memory "
              "WHERE project_id = ? AND id = ?");
  q.bind(1, pid);
  q.bind(2, id);
  if (!q.step()) throw NotFoundError("no memory with id " + std::string(id));
  return Row{q.text(0), q.text(1), q.text(2), q.integer(3), q.text(4), q.text(5), q.text(6)};
}

void check_revision(const Row& row, std::int64_t expected) {
  if (row.revision != expected) {
    throw ConflictError("revision conflict: expected " + std::to_string(expected) +
                        ", current is " + std::to_string(row.revision));
  }
}

// Every change, of content or state, bumps the revision and appends history.
void apply_change(Database& db, const std::string& pid, const Row& row,
                  std::string_view content, std::string_view state,
                  std::string_view actor, std::string_view reason,
                  const std::string& now) {
  Statement upd(
      db,
      "UPDATE memory SET content = ?, state = ?, revision_no = revision_no + 1, "
      "updated_at = ? WHERE project_id = ? AND id = ? AND revision_no = ?");
  upd.bind(1, content);
  upd.bind(2, state);
  upd.bind(3, now);
  upd.bind(4, pid);
  upd.bind(5, row.id);
  upd.bind(6, row.revision);
  upd.step();
  Statement changes(db, "SELECT changes()");
  changes.step();
  if (changes.integer(0) != 1)
    throw ConflictError("revision conflict while writing " + row.id);

  const std::int64_t next = row.revision + 1;
  Statement rev(db,
                "INSERT INTO memory_revision (memory_id, revision_no, content, state, "
                "actor, reason, "
                "changed_at) VALUES (?, ?, ?, ?, ?, ?, ?)");
  rev.bind(1, row.id);
  rev.bind(2, next);
  rev.bind(3, content);
  rev.bind(4, state);
  rev.bind(5, actor);
  rev.bind(6, reason);
  rev.bind(7, now);
  rev.step();

  if (state != row.state) {
    Statement ev(db,
                 "INSERT INTO memory_state_event (memory_id, from_state, to_state, "
                 "actor, reason, at) "
                 "VALUES (?, ?, ?, ?, ?, ?)");
    ev.bind(1, row.id);
    ev.bind(2, row.state);
    ev.bind(3, state);
    ev.bind(4, actor);
    ev.bind(5, reason);
    ev.bind(6, now);
    ev.step();
  }
}

void transition_in(Database& db, const Clock& clock, const std::string& pid,
                   const Row& row, std::int64_t expected, std::string_view to,
                   std::string_view actor, std::string_view reason) {
  check_revision(row, expected);
  require(row.state != "archived", "archived memories cannot change state");
  require(contains(kStates, to), "unknown state");
  require(!reason.empty(), "a reason is required for every state change");
  require(transition_allowed(row.state, to), "illegal state transition");
  apply_change(db, pid, row, row.content, to, actor, reason, to_iso8601(clock.now()));
}

Memory read_memory(Database& db, const std::string& pid, std::string_view id) {
  Statement q(db,
              "SELECT id, type, content, state, scope, author, revision_no, "
              "COALESCE(superseded_by, ''), created_at, updated_at, approved_by FROM memory "
              "WHERE project_id = ? AND id = ?");
  q.bind(1, pid);
  q.bind(2, id);
  if (!q.step()) throw NotFoundError("no memory with id " + std::string(id));
  return Memory{q.text(0), q.text(1),    q.text(2), q.text(3), q.text(4),
                q.text(5), q.integer(6), q.text(7), q.text(8), q.text(9), q.text(10)};
}

}  // namespace

std::string create_memory(Database& db, const Clock& clock, IdGenerator& ids,
                          const std::filesystem::path& root, const NewMemory& input) {
  require(contains(kTypes, input.type), "unknown memory type");
  require(contains(kScopes, input.scope), "unknown scope");
  require(!input.author.empty(), "an author is required for attribution");
  require(!input.content.empty(), "content must not be empty");

  const std::string pid = project_of(root);
  const std::string now = to_iso8601(clock.now());
  const std::string id = ids.next();

  Transaction tx(db);
  ensure_project(db, root, pid, now);

  Statement ins(
      db,
      "INSERT INTO memory (id, project_id, type, content, state, scope, author, "
      "revision_no, superseded_by, created_at, updated_at) "
      "VALUES (?, ?, ?, ?, 'active', ?, ?, 1, NULL, ?, ?)");
  ins.bind(1, id);
  ins.bind(2, pid);
  ins.bind(3, input.type);
  ins.bind(4, input.content);
  ins.bind(5, input.scope);
  ins.bind(6, input.author);
  ins.bind(7, now);
  ins.bind(8, now);
  ins.step();

  Statement rev(db,
                "INSERT INTO memory_revision (memory_id, revision_no, content, state, "
                "actor, reason, "
                "changed_at) VALUES (?, 1, ?, 'active', ?, 'created', ?)");
  rev.bind(1, id);
  rev.bind(2, input.content);
  rev.bind(3, input.author);
  rev.bind(4, now);
  rev.step();

  Statement ev(db,
               "INSERT INTO memory_state_event (memory_id, from_state, to_state, actor, "
               "reason, at) "
               "VALUES (?, 'none', 'active', ?, 'created', ?)");
  ev.bind(1, id);
  ev.bind(2, input.author);
  ev.bind(3, now);
  ev.step();

  // Evidence must name a symbol the project has indexed. A reference that does
  // not resolve rejects the whole memory: the transaction rolls back.
  for (const EvidenceRef& ref : input.evidence) {
    Statement find(
        db,
        "SELECT s.body_fingerprint FROM symbol s JOIN file f ON f.id = s.file_id "
        "WHERE f.project_id = ? AND f.rel_path = ? AND s.fqn = ? AND s.kind != 'module' "
        "ORDER BY s.start_line LIMIT 1");
    find.bind(1, pid);
    find.bind(2, ref.rel_path);
    find.bind(3, ref.symbol_fqn);
    if (!find.step()) {
      throw Error("invalid evidence: " + ref.rel_path + " has no indexed symbol " +
                  ref.symbol_fqn);
    }
    Statement add(db,
                  "INSERT INTO evidence (memory_id, project_id, rel_path, symbol_fqn, "
                  "content_fingerprint) VALUES (?, ?, ?, ?, ?)");
    add.bind(1, id);
    add.bind(2, pid);
    add.bind(3, ref.rel_path);
    add.bind(4, ref.symbol_fqn);
    add.bind(5, find.text(0));
    add.step();
  }

  tx.commit();
  return id;
}

std::optional<Memory> get_memory(Database& db, const std::filesystem::path& root,
                                 std::string_view id, std::string_view viewer) {
  try {
    Memory m = read_memory(db, project_of(root), id);
    if (!visible_to(m.scope, m.author, m.approved_by, viewer)) return std::nullopt;
    return m;
  } catch (const Error&) {
    return std::nullopt;
  }
}

std::vector<EvidenceRow> evidence_of(Database& db, const std::filesystem::path& root,
                                     std::string_view id) {
  Statement q(db,
              "SELECT e.rel_path, e.symbol_fqn, e.content_fingerprint FROM evidence e "
              "JOIN memory m ON m.id = e.memory_id "
              "WHERE m.project_id = ? AND e.memory_id = ? ORDER BY e.id");
  q.bind(1, project_of(root));
  q.bind(2, id);
  std::vector<EvidenceRow> out;
  while (q.step()) out.push_back(EvidenceRow{q.text(0), q.text(1), q.text(2)});
  return out;
}

std::vector<Memory> list_memories(Database& db, const std::filesystem::path& root,
                                  std::string_view state, std::string_view viewer) {
  const std::string pid = project_of(root);
  Statement q(db,
              state.empty()
                  ? "SELECT id FROM memory WHERE project_id = ? ORDER BY created_at, id"
                  : "SELECT id FROM memory WHERE project_id = ? AND state = ? ORDER BY "
                    "created_at, id");
  q.bind(1, pid);
  if (!state.empty()) q.bind(2, state);
  std::vector<std::string> ids;
  while (q.step()) ids.push_back(q.text(0));
  std::vector<Memory> out;
  out.reserve(ids.size());
  for (const std::string& id : ids) {
    Memory m = read_memory(db, pid, id);
    if (visible_to(m.scope, m.author, m.approved_by, viewer)) out.push_back(std::move(m));
  }
  return out;
}

void update_memory(Database& db, const Clock& clock, const std::filesystem::path& root,
                   std::string_view id, std::int64_t expected_revision,
                   std::string_view content, std::string_view actor,
                   std::string_view reason) {
  require(!content.empty(), "content must not be empty");
  require(!actor.empty(), "an actor is required for attribution");
  require(!reason.empty(), "a reason is required for every change");
  const std::string pid = project_of(root);
  Transaction tx(db);
  const Row row = load(db, pid, id);
  authorise_write(row, actor);
  check_revision(row, expected_revision);
  require(row.state != "archived", "archived memories are read-only");
  apply_change(db, pid, row, content, row.state, actor, reason, to_iso8601(clock.now()));
  if (!row.approved_by.empty()) {
    // The approved content is no longer the content that was approved (ADR-0017).
    Statement clear(db, "UPDATE memory SET approved_by = '' WHERE project_id = ? AND id = ?");
    clear.bind(1, pid);
    clear.bind(2, row.id);
    clear.step();
  }
  tx.commit();
}

void transition_memory(Database& db, const Clock& clock,
                       const std::filesystem::path& root, std::string_view id,
                       std::int64_t expected_revision, std::string_view to_state,
                       std::string_view actor, std::string_view reason) {
  const std::string pid = project_of(root);
  Transaction tx(db);
  const Row row = load(db, pid, id);
  authorise_write(row, actor);
  transition_in(db, clock, pid, row, expected_revision, to_state, actor, reason);
  tx.commit();
}

void supersede_memory(Database& db, const Clock& clock, const std::filesystem::path& root,
                      std::string_view old_id, std::int64_t expected_revision,
                      std::string_view new_id, std::string_view actor,
                      std::string_view reason) {
  require(old_id != new_id, "a memory cannot supersede itself");
  const std::string pid = project_of(root);
  Transaction tx(db);
  const Row old_row = load(db, pid, old_id);
  authorise_write(old_row, actor);
  load(db, pid, new_id);  // the replacement must exist in this project
  transition_in(db, clock, pid, old_row, expected_revision, "superseded", actor, reason);
  Statement link(db,
                 "UPDATE memory SET superseded_by = ? WHERE project_id = ? AND id = ?");
  link.bind(1, new_id);
  link.bind(2, pid);
  link.bind(3, old_id);
  link.step();
  tx.commit();
}

std::vector<Revision> revisions_of(Database& db, const std::filesystem::path& root,
                                   std::string_view id) {
  Statement q(db,
              "SELECT r.revision_no, r.content, r.state, r.actor, r.reason, r.changed_at "
              "FROM memory_revision r JOIN memory m ON m.id = r.memory_id "
              "WHERE m.project_id = ? AND r.memory_id = ? ORDER BY r.revision_no");
  q.bind(1, project_of(root));
  q.bind(2, id);
  std::vector<Revision> out;
  while (q.step()) {
    out.push_back(
        Revision{q.integer(0), q.text(1), q.text(2), q.text(3), q.text(4), q.text(5)});
  }
  return out;
}

std::vector<StateEvent> state_events_of(Database& db, const std::filesystem::path& root,
                                        std::string_view id) {
  Statement q(db,
              "SELECT e.from_state, e.to_state, e.actor, e.reason, e.at "
              "FROM memory_state_event e JOIN memory m ON m.id = e.memory_id "
              "WHERE m.project_id = ? AND e.memory_id = ? ORDER BY e.id");
  q.bind(1, project_of(root));
  q.bind(2, id);
  std::vector<StateEvent> out;
  while (q.step()) {
    out.push_back(StateEvent{q.text(0), q.text(1), q.text(2), q.text(3), q.text(4)});
  }
  return out;
}

namespace {

// The fingerprint of a named, non-module symbol in this project, if it exists.
std::optional<std::string> fingerprint_of(Database& db, const std::string& pid,
                                          std::string_view rel_path,
                                          std::string_view fqn) {
  Statement q(
      db,
      "SELECT s.body_fingerprint FROM symbol s JOIN file f ON f.id = s.file_id "
      "WHERE f.project_id = ? AND f.rel_path = ? AND s.fqn = ? AND s.kind != 'module' "
      "ORDER BY s.start_line LIMIT 1");
  q.bind(1, pid);
  q.bind(2, rel_path);
  q.bind(3, fqn);
  if (!q.step()) return std::nullopt;
  return q.text(0);
}

// Every (path, name) whose body has this fingerprint.
std::vector<std::pair<std::string, std::string>> places_with(
    Database& db, const std::string& pid, std::string_view fingerprint) {
  Statement q(db,
              "SELECT f.rel_path, s.fqn FROM symbol s JOIN file f ON f.id = s.file_id "
              "WHERE f.project_id = ? AND s.body_fingerprint = ? AND s.kind != 'module'");
  q.bind(1, pid);
  q.bind(2, fingerprint);
  std::vector<std::pair<std::string, std::string>> out;
  while (q.step()) out.emplace_back(q.text(0), q.text(1));
  return out;
}

struct Cited {
  std::int64_t id;
  std::string rel_path;
  std::string symbol_fqn;
  std::string fingerprint;
};

std::vector<Cited> cited_by(Database& db, const std::string& pid,
                            const std::string& memory_id) {
  Statement q(db,
              "SELECT id, rel_path, symbol_fqn, content_fingerprint FROM evidence "
              "WHERE memory_id = ? AND project_id = ? ORDER BY id");
  q.bind(1, memory_id);
  q.bind(2, pid);
  std::vector<Cited> out;
  while (q.step()) out.push_back(Cited{q.integer(0), q.text(1), q.text(2), q.text(3)});
  return out;
}

}  // namespace

RevalidationStats revalidate_project(Database& db, const Clock& clock,
                                     const std::filesystem::path& root) {
  const std::string pid = project_of(root);
  RevalidationStats stats;

  std::vector<std::string> ids;
  {
    Statement q(
        db,
        "SELECT DISTINCT m.id FROM memory m JOIN evidence e ON e.memory_id = m.id "
        "WHERE m.project_id = ? AND m.state NOT IN ('superseded', 'invalidated', "
        "'archived') "
        "ORDER BY m.id");
    q.bind(1, pid);
    while (q.step()) ids.push_back(q.text(0));
  }

  Transaction tx(db);
  for (const std::string& id : ids) {
    const Row row = load(db, pid, id);
    ++stats.checked;

    bool changed = false;
    bool missing = false;
    for (const Cited& c : cited_by(db, pid, id)) {
      const auto current = fingerprint_of(db, pid, c.rel_path, c.symbol_fqn);
      if (current) {
        if (*current != c.fingerprint) changed = true;
        continue;
      }
      // The cited name is gone. If its exact body exists exactly once elsewhere,
      // it was renamed or moved: follow it and keep the fingerprint (ADR-0014).
      const auto places = places_with(db, pid, c.fingerprint);
      if (places.size() == 1) {
        Statement move(db,
                       "UPDATE evidence SET rel_path = ?, symbol_fqn = ? WHERE id = ?");
        move.bind(1, places[0].first);
        move.bind(2, places[0].second);
        move.bind(3, c.id);
        move.step();
        ++stats.reanchored;
      } else {
        missing = true;
      }
    }

    if (missing) {
      if (row.state == "active") {
        transition_in(db, clock, pid, row, row.revision, "evidence_missing", "system",
                      "a cited symbol could not be found");
        ++stats.flagged;
      }
    } else if (changed) {
      if (row.state == "active") {
        transition_in(db, clock, pid, row, row.revision, "suspected_stale", "system",
                      "a cited symbol changed");
        ++stats.flagged;
      }
    } else if (row.state == "suspected_stale" || row.state == "evidence_missing") {
      transition_in(db, clock, pid, row, row.revision, "active", "system",
                    "cited symbols match again");
      ++stats.restored;
    }
  }
  tx.commit();
  return stats;
}

void accept_memory(Database& db, const Clock& clock, const std::filesystem::path& root,
                   std::string_view id, std::int64_t expected_revision,
                   std::string_view actor, std::string_view reason) {
  require(!actor.empty(), "an actor is required for attribution");
  require(!reason.empty(), "a reason is required for every change");
  const std::string pid = project_of(root);
  Transaction tx(db);
  const Row row = load(db, pid, id);
  authorise_write(row, actor);
  check_revision(row, expected_revision);
  require(row.state == "suspected_stale" || row.state == "evidence_missing",
          "only a flagged memory can be accepted");

  for (const Cited& c : cited_by(db, pid, row.id)) {
    const auto current = fingerprint_of(db, pid, c.rel_path, c.symbol_fqn);
    require(current.has_value(),
            "a cited symbol is still missing; it cannot be accepted");
    Statement refresh(db, "UPDATE evidence SET content_fingerprint = ? WHERE id = ?");
    refresh.bind(1, *current);
    refresh.bind(2, c.id);
    refresh.step();
  }
  transition_in(db, clock, pid, row, expected_revision, "active", actor, reason);
  tx.commit();
}

void approve_memory(Database& db, const Clock& clock, const std::filesystem::path& root,
                    std::string_view id, std::int64_t expected_revision, std::string_view approver,
                    std::string_view reason) {
  require(!approver.empty(), "an approver is required for attribution");
  require(!reason.empty(), "a reason is required for every approval");
  const std::string pid = project_of(root);
  Transaction tx(db);
  const Row row = load(db, pid, id);
  // Approval does not need visibility: an unapproved approved-scope memory is hidden
  // from other agents, and the approver must still be able to act on it.
  check_revision(row, expected_revision);
  require(row.scope == "approved", "only an approved-scope memory takes an approval");
  require(row.state != "archived", "archived memories cannot be approved");
  require(approver != row.author, "the author cannot approve their own memory");
  apply_change(db, pid, row, row.content, row.state, approver, "approved: " + std::string(reason),
               to_iso8601(clock.now()));
  Statement mark(db, "UPDATE memory SET approved_by = ? WHERE project_id = ? AND id = ?");
  mark.bind(1, approver);
  mark.bind(2, pid);
  mark.bind(3, row.id);
  mark.step();
  tx.commit();
}

std::size_t purge_archived(Database& db, const std::filesystem::path& root,
                           Timestamp cutoff) {
  const std::string pid = project_of(root);
  const std::string limit = to_iso8601(cutoff);
  Transaction tx(db);
  // Links from superseding memories to the purged ones are cleared first, so
  // the delete cannot leave a dangling reference.
  Statement unlink(db,
                   "UPDATE memory SET superseded_by = NULL WHERE superseded_by IN "
                   "(SELECT id FROM memory WHERE project_id = ? AND state = 'archived' "
                   "AND updated_at < ?)");
  unlink.bind(1, pid);
  unlink.bind(2, limit);
  unlink.step();
  Statement del(db,
                "DELETE FROM memory WHERE project_id = ? AND state = 'archived' AND "
                "updated_at < ?");
  del.bind(1, pid);
  del.bind(2, limit);
  del.step();
  Statement changes(db, "SELECT changes()");
  changes.step();
  const auto removed = static_cast<std::size_t>(changes.integer(0));
  tx.commit();
  return removed;
}

}  // namespace veylqora
