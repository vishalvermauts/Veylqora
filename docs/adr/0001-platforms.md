# ADR-0001 — Build and reference platforms

- **Status:** accepted
- **Date:** 2026-10-20

## Context

The host development machine runs Windows 11 and had no C++ toolchain, no CMake
and no package manager (see `PLAN_REVIEW.md` §0). Three mandated activities are
degraded or impossible on MSVC:

1. UndefinedBehaviorSanitizer does not exist on MSVC.
2. libFuzzer support on Windows is partial.
3. Defender scanning and NTFS small-file latency make Windows a noisy
   benchmark host, and its numbers are not comparable to the Linux figures a
   reader of a benchmark report expects.

A Docker Cloud Sandbox is available and billable per running minute.

## Decision

- **Linux is the reference platform** for correctness builds, sanitizers,
  fuzzing and every published benchmark. Concretely: Ubuntu 26.04,
  gcc 15.2 and clang 21.1, verified in the `veylqora-build` cloud sandbox.
- **Windows is a supported build target**, exercised in CI, but no benchmark or
  sanitizer result is published from it.
- Compilation and test runs happen in the cloud sandbox. Because it is
  chargeable, it is **stopped whenever it is not actively building**, and
  sandbox sessions are batched rather than interactive.
- `scripts/sbx.sh` is the only supported entry point for sandbox use, so that
  starting and stopping are symmetric and no session is left running.

## Consequences

- `VEYLQORA_SANITIZER` fails fast on MSVC rather than silently producing an
  unsanitised binary.
- Benchmark reports must name the platform; a Windows number is never compared
  against a Linux number without saying so.
- Developers without the sandbox can still build and test on Windows; they
  cannot certify Modules 3.2 or 3.3.
