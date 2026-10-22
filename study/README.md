# M3.6b: agent study (Veylqora)

Ten seeded bugs in a public repository (pallets/click, BSD-3-Clause), each caught
by the repository's own test suite. Four setups per task, on gemini-2.5-flash
through a LiteLLM gateway, with a coding-agent CLI as the agent.

Setups: A built-in tools only; B + Veylqora code search; C + Veylqora memory
(seeded with four generic repository notes); D + Veylqora context package.

## Results

Full study (version 3 runner, 40 runs: 10 tasks x 4 setups, one run each):

| Setup | Passed |
|---|---|
| A (no Veylqora tools) | 5/10 |
| B (code search) | 4/10 |
| C (search + memory) | 6/10 |
| D (context package) | 3/10 |

- Veylqora tools were called 49 times in the 30 runs that had them.
- No run timed out, no gateway failure needed a retry, and no run changed a test file.
- Gateway cost for all 40 runs: about $1.51.

**Reading:** the tools work and the agent uses them. This study does not show that
they raise the pass rate. Ten bugs with one run each is too small: the differences
between setups are within chance. Task 5 failed in all four setups.

## Pilot (version 3 runner, 8 runs)

Before the full study, 8 runs checked the wiring: the key helper authenticates, the
served model is the one requested, and tool calls are recorded. Passed 5/8, no
infrastructure errors, cost about $0.19.

## Version 3 changes (from version 2)

- The prompt gives the reported symptom in words, not the names of failing test files.
- Hints use the real MCP tool names (`mcp__veylqora__...`).
- The API key reaches the agent CLI through `apiKeyHelper`, not an environment variable.
- Gateway or client failures are retried up to 3 times, each on a fresh copy. A row
  that still fails is recorded as an infrastructure error and is not scored.
- Served model comes from the stream. Cost is computed from usage at the gateway's
  published prices (input $0.30, cache read $0.03, output $2.50 per million tokens).
- Each run's temporary folder is removed, including on abort.
- Symptom text is hand-written from each seeded change. Its wording is the
  experimenter's own, so it may still hint at the location.

Version 2 (40 runs, earlier): inconclusive. Veylqora tools were called in only 2 of 40
runs, so it said little about the tools.

## Running it

The runner needs the sandbox, the seeded-bug list and the study key, none of which are
in this repository. Set the agent CLI location in the environment first:

```bash
STUDY_AGENT_CLI=/path/to/agent-cli python3 run_study.py pilot   # 8 runs
STUDY_AGENT_CLI=/path/to/agent-cli python3 run_study.py part1   # tasks 0,2,3,4,5
STUDY_AGENT_CLI=/path/to/agent-cli python3 run_study.py part2   # tasks 6,7,8,11,12
```

Raw results are kept outside the repository (root-only folder in the sandbox, and a
local private folder), because they contain the answers to the seeded bugs.

## Not done

- **Stale-memory test.** Seed memories on the clean tree, apply the bug, re-index, and
  check that memories citing changed code are flagged. Needs care so the seeded notes do
  not point at the bug site.
- **Larger study.** Next step: more tasks and more runs per cell, on a bigger codebase.

## Limits

10 tasks and one run per cell are far too few for statistical significance. Seeded
bugs are not representative of real defects. Results are indicative only.
