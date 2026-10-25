# staleness-replay study scripts

CPU-only study scripts for the Veylqora memory-staleness question: does
Veylqora flag memories as `suspected_stale` when the code they cite really
changes, and does it stay quiet when it does not? These scripts are the
independent ground-truth side. They are written and committed BEFORE any
Veylqora build or run; nothing here imports veylqora, calls any model, or
touches the network. Labels are re-derived from the fossil git history of
the subject repositories with `git` and the Python `ast` module only.

Pipelines (run in this order):

    python3 label_history.py  <repo_dir> <out_labels.json>
    python3 build_cases.py    --labels c1.json c2.json c3.json --out cases.json
    python3 replay_staleness.py --cases cases.json --repos-root <dir> \
        --results <results_dir> --vey <vey_binary> [--pilot]
    python3 score_staleness.py --cases cases.json --steps steps.jsonl \
        --cases-rows cases.jsonl --repos-root <dir> --lineages <dir> \
        [--out report.json]

The four docstrings below are the pre-registered rules of this study. They
were fixed before any subject build or run and are restated verbatim in
docs/research/staleness-replay-summary.md. Any deviation found during
execution must be reported as a deviation, never baked in silently.

PASS RULE (from score_staleness.py, verbatim): "precision >= 0.85 and
recall >= 0.85 on the body-change cases" - core pool = body_change +
unchanged_neighbours_change, predicted positive = ever suspected_stale or
evidence_missing within the window, precision and recall as defined in the
SCORING RULE.

QUOTAS (from build_cases.py, verbatim): at least 60 core cases (a+b) across
3 repos with at least 15 per repo and both types represented; at least 5
renamed, at least 3 moved, at least 8 deleted, at least 8 callee_only;
window cap 20 commits after t0; the validity filter is the `matches` field
of the LABEL RULE: renamed valid iff matches == 1, moved valid iff
matches == 1, deleted valid iff matches == 0; excluded-with-note counts are
reported, never silently dropped.

Honesty rules, fixed before running:

  * Whatever the numbers are - including a fail of the PASS RULE - the
    summary reports them.
  * Every failure (failed index, failed memory add, unparsable blob,
    missing case) is written to the results rows, never dropped.
  * Nothing but these scripts (and docs/research/
    staleness-replay-summary.md, last phase) is committed. Raw per-case
    rows, labels and case lists stay out of the repository: they are
    scratch evidence of this run only, not study history.
  * No commits are made on any branch other than the study branch, and no
    merges or PRs. The study scripts and summary stay inside
    scripts/staleness_replay/ and docs/research/.
  * The seed 20261024 is recorded in every artifact.

File map:

  label_history.py     independent labeller; LABEL RULE in its docstring
  build_cases.py       case selection; CASE RULE in its docstring
  replay_staleness.py  replay mechanics; REPLAY MECHANICS in its docstring
  score_staleness.py   scorer; SCORING RULE in its docstring
