# ADR-0018: Observability: query log, audit log, metrics, retention

Status: accepted (M2.7)

## Context

The dashboard needs retrieval statistics, and operators need to know who changed
shared memory. Both risk leaking what agents asked or wrote. A metric with no
backing row is a placeholder, so every metric must trace to stored rows.

## Decisions

1. **Query log stores a hash, not the query.** `query_log` keeps the SHA-256 of
   the query text, the method, result count, latency and project. Raw text is
   never stored, so the log cannot leak what an agent searched for.
2. **Sampling is by hash.** A query is logged when the first eight hex digits of
   its hash are divisible by `query_log_every`. The decision is deterministic, so
   a given query is always logged or never logged. `0` turns the log off (opt-out).
3. **Audit log holds sensitive mutations only.** Memory create, update, approve,
   accept, transition, and index runs each write one row: actor, action, target
   kind and target id. No payload, content or query is stored. Reads are not audited.
4. **Retention is enforced at start and after index runs.** Query rows older than
   30 days and audit rows older than 365 days are deleted. Timestamps are
   fixed-width UTC, so retention compares them as strings. Tests age rows with a
   fixed clock.
5. **Metrics are counts over rows.** `GET /api/v1/metrics?root=` returns counts
   computed from `memory`, `file`, `symbol`, `query_log` and `audit_log`. No
   in-memory counters are kept, so every value traces to a row. The audit log has
   no project column, so its per-action counts are instance-wide; the response
   says so.

## Deviations from the plan's schema (§6.1)

- Row ids are `INTEGER PRIMARY KEY AUTOINCREMENT`, not `TEXT`. They are never
  exposed as identities, so a sequence is simpler and sorts by insertion time.
- `audit_log` has no `project_id`, because the plan's table has none. Adding one
  would make audit rows for index runs and memories ambiguous across projects.
  Instance-wide counts are the honest result of that.

## Consequences

- Query text is unrecoverable from the log. Debugging a bad query needs the
  caller to reproduce it.
- Audit rows are written after a successful mutation, not in the same
  transaction. A crash between the two loses the audit row, but never writes one
  for a mutation that did not happen.
- `serve --query-log-every N` sets the sampling rate. The default logs every query.
