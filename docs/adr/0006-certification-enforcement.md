# ADR-0006 — Certification is enforced by scripts, not written by hand

- **Status:** accepted
- **Date:** 2026-10-20

## Context

The directive asked that certification be enforced technically. A PASS typed
into a Markdown file proves nothing, and neither does a script that only reads
one.

## Decision

- `scripts/certify.py <module>` runs the module's mandatory checks at HEAD and
  writes `docs/certification/<module>/<sha>/manifest.json` plus each check's log.
- It refuses a dirty tree and an unpushed HEAD, so the manifest names exactly
  what was tested. A check that cannot run counts as a failure.
- Status is PASS only if every check exits 0; PARTIAL if some do; otherwise FAIL.
- Module sources are recorded by git blob id. Changing any of them makes the
  manifest stale. Blob ids are used rather than file hashes because Windows
  checkouts convert line endings, and git's ids do not change with them.
- `scripts/verify_certs.py` rejects a manifest whose content no longer matches
  its digest (hand edits), a log that is missing, and a PASS with a failed
  check. Staleness is a warning, and with `--strict` (release gate) it fails
  when no PASS manifest matches HEAD. The `certs` job in `scripts/ci.sh` runs it.
- Evidence logs are committed. A `.gitignore` rule that hid them was removed.

## Consequences

- Certifying a module takes a sandbox run. Re-certification is required after
  any change to its sources.
- Tests are counted from ctest output; a check whose output format changes will
  show no test counts, which the manifest makes visible rather than hiding.
- Known gap: the manifest digest is a tamper-evidence check, not a signature.
  Anyone who can rewrite the repo can rewrite the manifest too. Signing is out
  of scope until there is a release process that needs it.
