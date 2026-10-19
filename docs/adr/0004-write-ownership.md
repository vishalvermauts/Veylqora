# ADR-0004 — Write ownership across CLI, API and MCP

- **Status:** accepted
- **Date:** 2026-10-20

## Context

Three entry points touch durable state: the CLI, the HTTP API and the MCP
server. SQLite in WAL mode tolerates many readers and one writer across
processes. A USearch index file has **no multi-process locking**: if the CLI
mutates the index while the server holds it, the result is corruption that no
test in the original plan would have detected.

## Decision

- **The server process is the single writer** of the SQLite database and of
  every index file. All schema migrations run there.
- **The CLI is a thin client** over the local API. It does not open the index.
- `--direct` exists for read-only inspection when no server is running. It
  acquires an exclusive lock file (`<data_dir>/veylqora.lock`) and opens SQLite
  read-only; it refuses to run if the lock is held.
- **The MCP server is a client**, never a parallel writer. It speaks stdio to
  the agent and HTTP/JSON to the server.
- Warnings-as-errors policy, bounded worker pools and request limits apply at
  the server boundary, which is now the only place state changes.

## Consequences

- `veylqora index` requires a running server, or starts an ephemeral one with
  the lock held. This must be stated in the CLI help, not discovered.
- A corruption test belongs in the regression suite: two processes attempting
  concurrent index mutation must fail cleanly, not interleave.
- Single-writer is a deliberate ceiling on write throughput. It is adequate for
  a single-node developer tool and is the reason no distributed consensus
  machinery appears anywhere in the design.
