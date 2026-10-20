#pragma once

#include "veylqora/core/error.hpp"
#include "veylqora/core/runtime.hpp"
#include "veylqora/storage/database.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veylqora {

/// An update was based on a revision that is no longer current. Nothing was
/// written. The caller re-reads the memory and retries (ADR-0011).
class ConflictError : public Error {
 public:
  using Error::Error;
};

/// Points at a symbol the project has indexed. Evidence that names nothing
/// indexed is rejected, so a memory can never cite code that does not exist.
struct EvidenceRef {
  std::string rel_path;
  std::string symbol_fqn;
};

struct NewMemory {
  std::string type;  // decision, observation, task_outcome, failed_approach, requirement,
                     // constraint
  std::string content;
  std::string author;             // agent or user identity; required for attribution
  std::string scope = "private";  // private, shared, approved
  std::vector<EvidenceRef> evidence;
};

struct Memory {
  std::string id;
  std::string type;
  std::string content;
  std::string state;
  std::string scope;
  std::string author;
  std::int64_t revision_no;
  std::string superseded_by;  // empty unless superseded
  std::string created_at;
  std::string updated_at;
};

struct EvidenceRow {
  std::string rel_path;
  std::string symbol_fqn;
  std::string content_fingerprint;  // the symbol's body fingerprint when cited
};

struct Revision {
  std::int64_t revision_no;
  std::string content;
  std::string state;
  std::string actor;
  std::string reason;
  std::string changed_at;
};

struct StateEvent {
  std::string from_state;
  std::string to_state;
  std::string actor;
  std::string reason;
  std::string at;
};

/// Creates a memory with its evidence, atomically. Returns the new id.
std::string create_memory(Database& db, const Clock& clock, IdGenerator& ids,
                          const std::filesystem::path& root, const NewMemory& input);

/// The memory in this project, or nullopt. Another project's id is not found.
std::optional<Memory> get_memory(Database& db, const std::filesystem::path& root,
                                 std::string_view id);

std::vector<EvidenceRow> evidence_of(Database& db, const std::filesystem::path& root,
                                     std::string_view id);

/// Memories in this project, optionally filtered by state, ordered by creation.
std::vector<Memory> list_memories(Database& db, const std::filesystem::path& root,
                                  std::string_view state = "");

/// Replaces the content. `expected_revision` must be the current revision, or
/// ConflictError is thrown and nothing changes.
void update_memory(Database& db, const Clock& clock, const std::filesystem::path& root,
                   std::string_view id, std::int64_t expected_revision,
                   std::string_view content, std::string_view actor,
                   std::string_view reason);

/// Moves the memory to a new state if the transition is allowed (ADR-0011).
void transition_memory(Database& db, const Clock& clock,
                       const std::filesystem::path& root, std::string_view id,
                       std::int64_t expected_revision, std::string_view to_state,
                       std::string_view actor, std::string_view reason);

/// Marks `old_id` superseded by `new_id`. Both must be in this project.
void supersede_memory(Database& db, const Clock& clock, const std::filesystem::path& root,
                      std::string_view old_id, std::int64_t expected_revision,
                      std::string_view new_id, std::string_view actor,
                      std::string_view reason);

std::vector<Revision> revisions_of(Database& db, const std::filesystem::path& root,
                                   std::string_view id);
std::vector<StateEvent> state_events_of(Database& db, const std::filesystem::path& root,
                                        std::string_view id);

struct RevalidationStats {
  std::size_t checked = 0;     // memories with evidence that were examined
  std::size_t flagged = 0;     // moved to suspected_stale or evidence_missing
  std::size_t restored = 0;    // returned to active because the evidence matches again
  std::size_t reanchored = 0;  // evidence found at a new location with the same body
};

/// Compares every memory's cited symbols with the current index (ADR-0014).
/// Changes are applied as system transitions with a recorded reason. Nothing is
/// invalidated here: that needs a decision.
RevalidationStats revalidate_project(Database& db, const Clock& clock,
                                     const std::filesystem::path& root);

/// Accepts the current code for a flagged memory: refreshes the stored
/// fingerprints of its evidence and returns it to active, with the actor and
/// reason recorded. Fails if a cited symbol is still missing.
void accept_memory(Database& db, const Clock& clock, const std::filesystem::path& root,
                   std::string_view id, std::int64_t expected_revision, std::string_view actor,
                   std::string_view reason);

/// Deletes archived memories last changed before `cutoff`, with their history.
/// Retention is explicit: nothing is deleted automatically.
std::size_t purge_archived(Database& db, const std::filesystem::path& root,
                           Timestamp cutoff);

}  // namespace veylqora
