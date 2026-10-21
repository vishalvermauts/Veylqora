# M3.6b: paid agent study (Veylqora)

Ten seeded bugs in a public repository (pallets/click, BSD-3-Clause), each caught
by the repository's own test suite. Four setups, one run each per task (40 runs),
on gemini-2.5-flash through the helixgateway LiteLLM proxy. Claude Code CLI 2.1.293
as the agent. Per-run cap set to $1.00 in Claude Code's own estimate; real spend
was tracked on the gateway key (`Veylqora-test`, $10 cap).

Setups: A built-in tools only; B + Veylqora code search; C + Veylqora memory
(seeded with four generic repository notes); D + Veylqora context package.

Results (results/results.jsonl): valid passes out of 10 runs per setup:
A 2/10, B 1/10, C 3/10, D 4/10. Two runs edited test files and are not counted
as valid; both are recorded with tests_modified=true.

Limits: 10 tasks and one run per cell are far too few for statistical
significance. Results are indicative only. Some runs ended early (6 to 14 turns),
which was not investigated. The model was served through a gateway whose latency
varied during the study. Seeded bugs are not representative of real defects.
