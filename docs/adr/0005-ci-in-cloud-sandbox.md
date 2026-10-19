# ADR-0005 — CI runs in a dedicated cloud sandbox

- **Status:** accepted
- **Date:** 2026-10-20

## Context

The project was to use GitHub Actions. The owner has no Actions credit, and
the local PC Docker engine is not to be used. Only the Docker Cloud Sandbox
(billed per running minute) is available for compiling and testing.

## Decision

- CI is `scripts/ci.sh`, which runs the gating jobs (`format`, `gcc`, `clang`,
  `san`) in a second cloud sandbox, `shell/veylqora-ci`.
- That sandbox is separate from the development sandbox (`veylqora-build`), so
  each run starts from a clean clone of `origin/main` (or a named ref) and
  cannot pick up uncommitted or leftover state.
- The sandbox is stopped at the end of every run, including on failure. It is
  started only by a run, never left idle.
- The job list is the one the plan requires. There is no hosted matrix, so the
  Windows/MSVC build is not gated in CI; it is verified manually until a
  Windows host is available. This is a known gap, recorded here.

## Consequences

- A run costs sandbox minutes; `--keep` is the only way to leave it running and
  it prints a warning.
- Dependency downloads and apt installs happen on first use; the sandbox
  filesystem survives `stop`, so later runs skip provisioning work.
- `ci.sh` provisions the ASan/UBSan runtime (`libclang-rt-dev`); the sanitizer
  gate is the one most likely to regress on a toolchain update.
