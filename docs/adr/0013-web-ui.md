# ADR-0013 — Web UI: React, TypeScript and Vite, served by the daemon

- **Status:** accepted (M1.6)
- **Date:** 2026-10-20

## Context

The plan asks for a web dashboard in React and TypeScript, built with Vite (§2.1).
M1.6 is read-only: it shows what the daemon already holds and never writes.

## Decisions

1. **Stack, pinned by exact version** in `web/package.json`: React 19.2.0,
   Vite 7.1.12, TypeScript 5.9.3, Vitest 3.2.4 with jsdom 26.1.0 and Testing
   Library. Each version was published at least two weeks before it was pinned.
2. **Same origin.** The daemon serves the built files from `--web <dir>`, so
   the UI calls `/api/v1/...` without CORS. No second server is needed.
3. **Read-only.** The UI only issues GET requests. Writes stay with the CLI and
   the API until a later module adds them with tests.
4. **The UI holds no state of its own.** It sends the project root with each
   request, and every number on screen comes from a response. Loading, error,
   and empty states are shown for every view.
5. **Graph view is a list, not a canvas.** Dependents and dependencies with
   their resolution levels. An interactive graph library is deferred: the plan
   requires a permissively licensed one, and it is not needed for a read-only
   first view. This is a documented gap against plan §3.1, not a claim of parity.

## Consequences

- Build and tests run in the build sandbox, which has npm access. The CI
  sandbox does not, so UI checks are recorded as separate evidence, not in the
  gating `ci` job.
- The browser smoke test needs a real browser; that is recorded as not run in
  this environment unless it is.
