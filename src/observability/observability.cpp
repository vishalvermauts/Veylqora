#include "veylqora/observability/observability.hpp"

#include "veylqora/core/identity.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace veylqora {
bool query_sampled(std::string_view query_sha256, std::int64_t every) {
  if (every <= 0) return false;
  if (every == 1) return true;
  // The first eight hex digits of the hash are uniform enough to sample by.
  std::uint64_t head = 0;
  for (std::size_t i = 0; i < 8 && i < query_sha256.size(); ++i) {
    const char c = query_sha256[i];
    const std::uint64_t digit = c <= '9' ? static_cast<std::uint64_t>(c - '0')
                                         : static_cast<std::uint64_t>(c - 'a' + 10);
    head = head * 16 + digit;
  }
  return head % static_cast<std::uint64_t>(every) == 0;
}

void log_query(Database& db, const Clock& clock, const ObservabilityOptions& options,
               std::string_view project_id, std::string_view method,
               std::string_view query, std::int64_t result_count, double latency_ms) {
  const std::string hash = content_fingerprint(query);
  if (!query_sampled(hash, options.query_log_every)) return;
  // A query against a project that was never indexed has no project row to attach to.
  // Logging it would fail the foreign key, so there is nothing to record: skip it.
  Statement known(db, "SELECT 1 FROM project WHERE id = ?");
  known.bind(1, project_id);
  if (!known.step()) return;
  Statement insert(
      db,
      "INSERT INTO query_log (project_id, method, query_sha256, result_count, "
      "latency_ms, at) VALUES (?, ?, ?, ?, ?, ?)");
  insert.bind(1, project_id);
  insert.bind(2, method);
  insert.bind(3, hash);
  insert.bind(4, result_count);
  insert.bind(5, latency_ms);
  insert.bind(6, to_iso8601(clock.now()));
  insert.step();
}

void audit(Database& db, const Clock& clock, std::string_view actor,
           std::string_view action, std::string_view target_kind,
           std::string_view target_id) {
  Statement insert(db,
                   "INSERT INTO audit_log (actor, action, target_kind, target_id, at) "
                   "VALUES (?, ?, ?, ?, ?)");
  insert.bind(1, actor);
  insert.bind(2, action);
  insert.bind(3, target_kind);
  insert.bind(4, target_id);
  insert.bind(5, to_iso8601(clock.now()));
  insert.step();
}

PurgeStats purge_expired(Database& db, const Clock& clock,
                         const ObservabilityOptions& options) {
  const auto now = clock.now();
  const auto cutoff = [&](int days) {
    return to_iso8601(now - std::chrono::hours(24 * static_cast<std::int64_t>(days)));
  };
  PurgeStats stats;
  {
    Statement del(db, "DELETE FROM query_log WHERE at < ?");
    del.bind(1, cutoff(options.query_retention_days));
    del.step();
  }
  {
    Statement changes(db, "SELECT changes()");
    changes.step();
    stats.query_rows = changes.integer(0);
  }
  {
    Statement del(db, "DELETE FROM audit_log WHERE at < ?");
    del.bind(1, cutoff(options.audit_retention_days));
    del.step();
  }
  {
    Statement changes(db, "SELECT changes()");
    changes.step();
    stats.audit_rows = changes.integer(0);
  }
  return stats;
}

}  // namespace veylqora
