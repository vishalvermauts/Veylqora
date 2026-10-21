# Offline evaluation (M3.6a)

This measures two things without any language model: how well retrieval finds
the right definitions, and how well stale-memory detection flags memories whose
cited code really changed. Anyone can rerun it.

## Rerun it

Build the daemon, then point the script at it:

```bash
cmake -S . -B build -G Ninja && cmake --build build
VEYLQORA_BIN=build/src/veylqora python3 scripts/evaluate.py
```

It starts a local daemon on a free port, indexes the fixture through the HTTP
API, scores the datasets, and writes `evaluation/results.json`. Exit code 0
means every gate passed. The script uses only the Python standard library.

## Datasets

| File | What it holds | How the labels were made |
|---|---|---|
| `datasets/retrieval.json` | 8 queries with the definitions a developer would expect | Written by reading `tests/fixtures/python` before any ranking was run |
| `datasets/staleness.json` | 11 code-change scenarios, each with the memories expected to be flagged | Written from ADR-0014's definition of a meaningful change, before the first scored run |

Labels are plain data. To challenge a label, change the JSON and say why in the
commit message. The thresholds below are not changed to fit results.

## Gates (fixed before the first scored run)

| Measure | Threshold | Source |
|---|---|---|
| Hybrid retrieval, mean recall@10 | ≥ 0.90 | M2.1 gate |
| Stale detection, precision | ≥ 0.80 | ADR-0014 |
| Stale detection, recall | ≥ 0.70 | ADR-0014 |

## Measures reported without a gate

- Mean recall@10 for lexical-only and vector-only retrieval, and mean reciprocal
  rank for hybrid retrieval. These show where each part of the hybrid method
  contributes.
- Per-scenario stale states, so a wrong flag can be traced to one change.

## Limitations and threats to validity

- **Small datasets.** Eight queries and eleven scenarios over one fixture are
  enough to catch regressions and not enough to estimate performance on real
  codebases. A mean over eight queries has a wide confidence interval.
- **Labels are ours.** The same people wrote the code, the labels and the
  thresholds. Independent labels from another project would be stronger.
- **Synthetic fixture.** The Python fixture is small and tidy. Real repositories
  have longer files, dynamic dispatch, generated code and more name collisions.
- **Vector retrieval is not semantic.** It uses hashed identifier tokens
  (ADR-0015), so it cannot match a query to a definition that shares no words
  with it. The negative result is reported in `results.json`.
- **Stale detection covers the cases ADR-0014 names.** Superseded memories and
  conflicting memories are not in the dataset, because the system does not
  detect them.
- **No timing.** Latency is measured in M3.2, not here.

## Negative results

Every gate outcome is in `results.json`, including any scenario the system got
wrong. A wrong scenario is reported with its expected and actual states; it is
not removed from the dataset.
