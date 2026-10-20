#pragma once

#include "veylqora/core/runtime.hpp"
#include "veylqora/storage/database.hpp"

#include <cstdint>
#include <string_view>

namespace veylqora {

/// Observability settings (M2.7, ADR-0018). Nothing here stores raw query text or
/// request bodies; the query log keeps only a SHA-256 of the query.
struct ObservabilityOptions {
  /// Log one query in every N, chosen by the query's hash, so a given query is
  /// always logged or never logged. 1 logs everything; 0 turns the query log off.
  std::int64_t query_log_every = 1;
  int query_retention_days = 30;
  int audit_retention_days = 365;
};

/// True when a query with this SHA-256 falls in the sample.
bool query_sampled(std::string_view query_sha256, std::int64_t every);

/// Records one query in query_log if it is sampled. Only the hash is stored.
void log_query(Database& db, const Clock& clock, const ObservabilityOptions& options,
               std::string_view project_id, std::string_view method,
               std::string_view query, std::int64_t result_count, double latency_ms);

/// Records one sensitive mutation. The row holds the actor, the action and the
/// target's identifier only: never a payload, a query or memory content.
void audit(Database& db, const Clock& clock, std::string_view actor,
           std::string_view action, std::string_view target_kind,
           std::string_view target_id);

struct PurgeStats {
  std::int64_t query_rows = 0;
  std::int64_t audit_rows = 0;
};

/// Deletes rows older than their retention period. Called at daemon start and
/// after each index run.
PurgeStats purge_expired(Database& db, const Clock& clock,
                         const ObservabilityOptions& options);

}  // namespace veylqora
