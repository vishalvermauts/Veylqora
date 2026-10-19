# ADR-0003 — Symbol identity and evidence anchoring

- **Status:** accepted
- **Date:** 2026-10-20

## Context

The directive described stable symbol identity as "where possible" and left
evidence granularity undefined. Both are load-bearing for the product's only
novel feature — deciding which stored memories a code change puts in doubt.

Two failure modes bound the design:

- **Anchor too coarse** (file-level): every touch to a 2000-line file marks
  every memory about it as suspect. A false-positive storm trains the user to
  ignore the signal.
- **Anchor too fragile** (line ranges, or identity that includes body text):
  renaming a function, moving it between files, or reformatting it destroys the
  link between a memory and its evidence. Memories silently lose provenance on
  every refactor.

## Decision

**Identity and content are separate, and identity excludes position.**

```
SymbolKey = (project, language, path, fqn, kind, overload)
symbol_id = sha256(canonical_join(SymbolKey, '\x1f'))
body_fingerprint = sha256(normalise(body))      -- stored separately
```

- Line numbers are never part of identity. They are stored as hints only.
- `body_fingerprint` normalises line endings and trailing whitespace, so
  reformatting does not trigger revalidation; interior blank lines are kept,
  because collapsing them would merge genuinely different bodies.
- The separator is `\x1f`, which cannot appear in a path, qualified name or
  language tag, so `("py", "thon/a.py")` and `("python", "a.py")` cannot
  collide. There is a test for exactly this.

**Evidence anchors at symbol granularity with a content fingerprint:**

```
evidence = (memory_id, symbol_id, content_fingerprint, commit_sha, line_hint?)
```

Revalidation is then one query, not a heuristic:

```sql
SELECT DISTINCT m.id
FROM memory m
JOIN evidence e ON e.memory_id = m.id
JOIN symbol   s ON s.id = e.symbol_id
WHERE m.state = 'active'
  AND s.body_fingerprint <> e.content_fingerprint;
```

**Rename and move are detected, not deleted.** After each parse:

| Situation | Classification | Effect on evidence |
|---|---|---|
| id present, fingerprint equal | `unchanged` | none |
| id present, fingerprint differs | `body_changed` | → `suspected_stale` |
| id absent, fingerprint matches a disappeared id | `renamed`/`moved` | re-anchor, record `renamed_from`, no revalidation |
| id absent, no fingerprint match | `deleted` | → `evidence_missing` (distinct from invalid) |
| new id, no fingerprint match | `added` | none |

`evidence_missing` is deliberately not the same as `invalidated`: a deleted
symbol means we can no longer check the memory, which is a different fact from
the memory being wrong.

## Consequences

- Rename detection is fingerprint-based, so a rename that also changes the body
  looks like delete + add. This is accepted for v0.1 and recorded as a known
  limitation; a similarity threshold is a later refinement, not a v0.1 feature.
- `overload` must be assigned deterministically by the parser (source order
  within a file) or ids are unstable across re-parses.
- Path canonicalisation (`/` separators, NFC, project-relative) is part of
  identity, so the canonicaliser is a security-relevant and correctness-relevant
  component and gets property-based tests.
