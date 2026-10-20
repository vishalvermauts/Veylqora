# ADR-0017 — Multi-agent coordination: visibility, write rights, approval, concurrency

- **Status:** accepted; thresholds fixed before the concurrency test was run
- **Date:** 2026-10-20

## Context

Several agents may share one project's memories (plan §2.6). They must not lose
each other's writes, must not read what is private to another agent, and must
not treat an unapproved decision as shared knowledge.

## Decisions

1. **Visibility by scope.** A `private` memory is visible only to its author. A
   `shared` memory is visible to every agent. An `approved` memory is visible
   to other agents only after an approval is recorded. A request that names no
   agent (the local operator, through the CLI) sees everything.

2. **Write rights.** Only the author may change a `private` memory. A `shared`
   memory can be changed by any agent that can see it, through the revision
   check. An `approved` memory is changed by any agent that can see it, and any
   change clears its approval, because the approved content is no longer the
   content that was approved.

3. **Approval.** An approval names an approver who is not the author. It is
   recorded with the revision it applies to and with a reason.

4. **Concurrency.** Every change is a compare-and-swap on the revision (ADR-0011).
   Concurrent writers retry on conflict; a write is never applied to a revision
   the writer did not read. The database's busy timeout turns brief lock
   contention into a wait, not an error.

5. **Refusal.** Reading a memory another agent may not see is reported as not
   found (no existence leak). Writing one that another agent may not change is
   refused as forbidden (HTTP 403).

6. **Auditable.** Every write and approval appends to the revision history and
   records the actor, so a shared memory's full history shows who changed it.

## Thresholds (fixed before the concurrency test was run)

- 40 agents, each making 5 successful updates to one shared memory from
  separate database connections, retrying on conflict:
  - lost updates: 0;
  - final revision = 1 + 200;
  - history rows = 1 + 200, with every actor's update present.
- A private memory is invisible to a second agent in listing and in lookup.
- An approved memory is invisible to other agents until approved, and invisible
  again after a later change.

## Not covered

- Agents on different machines: the daemon is single-node (ADR-0004).
- Authentication: agent identity is declared by the caller, not verified. The
  daemon is loopback-only, so this is a coordination guard, not a security
  boundary; ADR-0012 covers authentication.
