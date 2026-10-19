# ADR-0008 — Scope of M1.1 and deliberate deviations from the plan

- **Status:** accepted
- **Date:** 2026-10-20

## Context

M1.1 is meant to deliver the core runtime and the first vertical slice: index
ten Python files, search by symbol name, restart, and get the same answer. The
plan's full M1.1 list is larger than that slice needs. Building all of it now
would add code the slice does not exercise, which §0.3 of the directive forbids.

## Decision

The slice is built now. Each item below is either deferred to the module that
needs it, or implemented in a simpler form than the plan describes.

| Plan item (M1.1) | What M1.1 does | Deferred to / reason |
|---|---|---|
| `Clock`, `IdGenerator`, `FileSystem` injection | `Clock` only (used for `project.created_at`) | `IdGenerator` in M1.4, where memories need ids. `FileSystem` in M1.2, where the walker needs a seam for tests. |
| `std::expected` error channel | `veylqora::Error` exception | C++20 has no `std::expected`. Exceptions are used for expected failures only. |
| Config precedence (flags > env > files) | Command-line flags only | Config files and env vars arrive with the daemon (M1.5). |
| `veylqora doctor` with provenance | Not implemented | M1.5. |
| Logging | `log::warn` to stderr | Levels and structured output in M1.5. |
| Migration runner | Forward-only, transactional, `schema_version` table | — implemented as planned. |
| Threat model | `docs/SECURITY.md` | — implemented as planned. |
| Python extraction via tree-sitter | Indentation-based extractor | Tree-sitter is M1.3a, with precision/recall measured against labelled fixtures. The slice needs only names and line ranges, which the simple extractor gives. |
| Migration v1→head test in CI | Not yet | Added when there is a second migration to test against. |
| Version information | `veylqora --version` from CMake's `PROJECT_VERSION` | — implemented. |

Other decisions in this slice:

- **Identity.** `project_id` is the content fingerprint of the canonical root
  path. Re-indexing the same tree always finds the same project; the same
  content under a different path is a different project.
- **File id.** Deterministic from `(project_id, rel_path)`, so re-indexing
  updates rows in place and never orphans symbols.
- **Symbols are replaced per file.** A changed file has its symbols deleted and
  rewritten inside the same transaction. Unchanged files are not parsed.
- **Writer.** The CLI opens the database and writes directly. This violates the
  eventual single-writer rule (ADR-0004) on purpose, for one process at a time.
  It is removed when the daemon exists in M1.5.

## Known limitations of the extractor (recorded, not hidden)

- A `def` or `class` line inside a multi-line string is treated as code.
- Decorators are not part of a symbol's start line.
- Code with tab indentation mixed with spaces may be mis-nested.
- Nothing is resolved: no imports, no calls, no references. Those are M1.3a.

## Consequences

- The slice is smaller than the plan's M1.1 list. The skipped items are
  listed above with the module that now owns each one.
- Module 0.1's certified sources overlap with this module's code, so 0.1 is
  stale after this change. That is correct: it must be re-certified before a
  strict release check. Non-strict CI reports it as a warning.
