# Held-out file-localisation rerun

Reruns the offline file-localisation comparison reported in the README: for each
real GitHub issue, check out the repository at the issue's base commit, index it
with Veylqora, rank files for the issue text, and see where the file the real fix
changed lands. It compares Veylqora (lexical, vector and hybrid retrieval) with
two non-LLM baselines: BM25 over whole files, and an IDF-weighted identifier
match, which is roughly what a grep-using agent does. No language model is called.

This directory contains the code only. The instance results, the selection and
the run logs are study data and are not part of the repository.

## What it needs

1. **A built `veylqora` binary**, with the daemon (`veylqora serve`). Point
   `VEYLQORA_BIN` at it; the default is `~/ci-work/build-gcc/src/veylqora`,
   which is where `scripts/ci.sh gcc` leaves it inside the CI sandbox.
2. **The public SWE-bench Lite test split** as JSON: the same file SWE-bench
   publishes as `test.json`. Each row needs `instance_id`, `repo`,
   `base_commit`, `problem_statement` and `patch`.
3. **An instance-ID list** (optional): one `instance_id` per line, selecting
   which instances of that dataset to run. Nothing else about the run is stored
   here.
4. **Network access to GitHub**: repositories are cloned with
   `git clone --filter=blob:none`.
5. Python 3.9 or newer. No third-party packages.

## How to run it

```bash
export VEYLQORA_BIN=/path/to/build/src/veylqora
export LOC_EVAL_WORK=/path/to/scratch          # clones and scratch databases

python3 scripts/heldout_rerun/loc_eval.py test.json out.jsonl --ids instance-ids.txt
python3 scripts/heldout_rerun/analyze_loc.py out.jsonl
```

`loc_eval.py` is resumable: instances already present in `out.jsonl` are skipped,
so an interrupted run continues where it stopped. It prints one line per instance
and appends one JSON record per instance, including the rank of the gold file for
every method, for both "all files" and "test files excluded".

`analyze_loc.py` prints accuracy at 1, 3, 5 and 10 and mean reciprocal rank per
method, with bootstrap intervals over instances, paired differences between
methods at the same ranks, per-repository breakdowns, and index-time medians. It
uses only `out.jsonl`; it does not touch the repositories again.

## Notes on reading the numbers

* Rows whose gold file is not among the candidate `.py` files are dropped from
  the analysis (the script says how many) because no ranking method can find a
  file that was not a candidate.
* Candidates are Python files listed by `git ls-files` at the base commit.
  Symlinks are skipped; no other filtering happens in `loc_eval.py`.
* Indexing uses a fresh database per instance, so index time includes database
  creation and is not an incremental-update measurement.
