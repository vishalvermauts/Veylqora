# ADR-0011 — Memory engine: revisions, transitions, evidence, retention

- **Status:** accepted (M1.4)
- **Date:** 2026-10-20

## Context

Agents need memories that persist, that show who wrote them, that can be
updated safely when two agents write at once, and that cannot cite code that
does not exist. Plan §2.3 and §6.2 set the states and the transition rules.

## Decisions

1. **Every change is a revision.** Creation, a content update and a state
   change each increment `revision_no` and append a row to `memory_revision`
   (content, state, actor, reason, time). History is never rewritten.

2. **Optimistic concurrency by revision.** A writer names the revision it read.
   The update is `WHERE revision_no = ?`. If nothing matched, the write is
   refused with `ConflictError` and the current revision is reported. There are
   no locks held across a read and a write, so a stalled agent cannot block
   others. Tested with two connections racing on one memory: exactly one write
   lands and the other is a conflict, never a lost update.

3. **Transitions are a closed table.** Allowed moves are the eight in plan §6.2,
   plus archiving from any state. Every other move is refused, and every
   transition needs a reason. A code change can therefore only raise a question
   (`active → suspected_stale`); reaching `invalidated` needs a later, explicit
   decision.

4. **Evidence must name an indexed symbol.** Creating a memory with evidence
   looks up the symbol by file and qualified name, copies its body fingerprint,
   and inserts the memory and its evidence in one transaction. An unknown
   reference rolls the whole creation back, so no partial memory is left.

5. **Evidence keeps its own record.** Evidence stores the file path, the
   qualified name and the fingerprint at the time of citing. It has no foreign
   key to the symbol, so it survives when the symbol is later deleted. That is
   what makes `evidence_missing` a distinct, detectable state rather than a lost
   link (ADR-0003).

6. **Attribution is required.** Every memory and every change has an actor. A
   memory without an author is refused.

7. **Projects are isolated at the query.** Each lookup filters on the project.
   A memory from another project is reported as not found, not forbidden, so
   existence is not revealed.

8. **Retention is explicit.** `purge_archived` deletes only archived memories
   older than a cutoff the caller chooses. It clears supersession links first,
   so no dangling reference remains. Nothing is deleted automatically.

9. **Export and import are whole-file operations.** The JSONL format is
   deterministic: export, import into an empty database, and export again gives
   byte-identical output (tested). Import is one transaction with deferred
   foreign-key checks, because a supersession can cite a memory written later in
   the file. A corrupt line rejects the whole file.

## Consequences

- The state machine is enforced in the engine, not in callers. Future modules
  (validity checks, M2.3) call the same transition function.
- Memory content is stored as given. Redaction of secrets before storage is
  still outstanding (SECURITY.md).
- The export format carries no vectors; they are rebuilt from the content.
