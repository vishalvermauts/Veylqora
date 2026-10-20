# ADR-0016 — Context packages: bounded, evidence-linked, warnings first

- **Status:** accepted; thresholds fixed before the labelled tasks were run
- **Date:** 2026-10-20

## Context

An agent gets a task and a limited prompt. The context it receives must be
bounded, must cite where each fact came from, must flag memories that may be
stale, and must not repeat itself (plan §2.4). The package is assembled by the
MCP server (ADR-0007), so the rules live there.

## Decisions

1. **Order of filling: warnings, then memories, then code.** Warnings are never
   dropped to make room for code. A memory whose cited code changed is shown
   with its warning, not hidden: hiding a doubtful memory is how an agent repeats
   work it already did.
2. **Budget.** The package uses about four characters per token. Each item is
   added whole or not at all. The response reports what was left out, per
   section, under `truncated`.
3. **Evidence references are resolved.** Each included memory carries its
   evidence (file and qualified name). A package is valid only if every reference
   names a symbol the daemon currently indexes.
4. **No duplicates.** Each code definition and each memory appears at most once.
5. **Only active memories are offered as knowledge.** Superseded, invalidated and
   archived memories are not included, because they were decided against.

## Thresholds (fixed before the labelled tasks were run)

On the labelled tasks in `mcp/tests/test_context.py`, with a 2000-token budget:

- mean recall of the labelled code definitions in the package ≥ 0.80;
- every evidence reference in every package resolves (100 %);
- every included memory whose cited code changed carries a warning (100 %);
- no code definition or memory appears twice in one package.

## Not covered

- The labelled tasks are few and drawn from the same small fixture as the
  retrieval labels. They show the packaging rules work, not that a package helps
  an agent. That measurement is the controlled comparison in M3.6.
- Token counts are estimates (four characters per token), not model tokenizers.
