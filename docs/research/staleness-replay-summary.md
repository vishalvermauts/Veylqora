# Veylqora memory-staleness replay — results summary

Study: "SUBAGENT A: memory staleness on real git history". CPU-only, no LLM,
no gateway, no network: the only components executed are the `vey` binary built
from the Veylqora tree, `git`, python3 (`ast`-based ground truth), and shell.
Seed fixed at 20261024 everywhere. Study branch: `research/staleness-replay`
(no merges, no PRs). Raw per-run data lives outside this repository, under
`C:\AI\study-private\staleness\` (and in the execution sandbox) — see
*"Artifacts and privacy"*.

## Question

Does Veylqora's memory layer flag memories as `suspected_stale` /
`evidence_missing` when the code a memory cites really changes on real git
histories — and does it stay quiet when it should? The three subject
repositories are fossil snapshots of click, jinja and pygments (first-parent
lineages of 1384, 1690 and 1846 commits from their frozen clone heads). Four
independent scripts re-derive ground truth from git + `ast` only; a replay
runner drives Veylqora across commit windows; a scorer applies one
pre-registered pass rule to the replay rows.

## Pre-registered rules (verbatim; fixed before any build or run)

The following rules were written into the four study scripts **before** the
Veylqora binary was built and before any case was constructed or replayed
(commits `f9a0aba` and `9996df4`; the fixes made later never changed rule
text — see the ledger). They are restated here verbatim from the committed
script docstrings and from `scripts/staleness_replay/README.md`.

### PASS RULE

```
"precision >= 0.85 and recall >= 0.85 on the body-change cases"
- core pool = body_change + unchanged_neighbours_change,
- predicted positive = ever suspected_stale or evidence_missing within the
  window,
- precision and recall as defined in the SCORING RULE.
```

### QUOTAS

```
at least 60 core cases (a+b) across 3 repos with at least 15 per repo and
both types represented; at least 5 renamed, at least 3 moved, at least 8
deleted, at least 8 callee_only; window cap 20 commits after t0; the validity
filter is the `matches` field of the LABEL RULE: renamed valid iff
matches == 1, moved valid iff matches == 1, deleted valid iff matches == 0;
excluded-with-note counts are reported, never silently dropped.
```

### LABEL RULE (label_history.py)

```
Walk the repo's first-parent lineage oldest -> newest
(`git rev-list --first-parent HEAD`, reversed). For every .py path identity
that exists at HEAD, reconstruct its change stream along the lineage from one
streamed `git log --first-parent --name-status` pass over the whole repo,
run with `-c diff.renames=copies`: `A` means the path is born at that commit,
`M` means the file version changes while the path stays, `D` ends a lifetime
(only ever reached for lifetimes that do not contain HEAD), and `R`/`C`
entries (status old -> new, e.g. click's switch to the src/ layout) carry the
identity across a move or copy of its file: score 100 carries identical bytes
(no re-parse), score < 100 carries a modified version. For every
(commit c, parent p) boundary of the identity, parse both file versions with
`ast` and catalogue every ast.FunctionDef / ast.AsyncFunctionDef /
ast.ClassDef anywhere in the module (nested and async definitions included)
by its module-qualified name `module.Class.method`, where module = rel path
with slashes replaced by dots and ".py" stripped ("/__init__.py" collapsed,
the same convention the subject indexer uses). The canonical body of a
definition is `ast.dump(node, include_attributes=False)` with the
definition's own `name` replaced by the literal "<fn>", so a rename alone is
not a body change; nothing else is removed: comments and blank lines are
inherently absent from ASTs while decorators, docstrings, signatures,
defaults, annotations and nested statements are all kept. A definition's
label on a boundary is:

  added          present at c, absent at p
  deleted        present at p, absent at c
  body_changed   present at both, canonical bodies differ
  name_changed   present at c under a new name whose canonical body equals
                 the canonical body of a definition present at p that is
                 absent at c (matched within the same file identity; the
                 partner definition keeps a `renamed_away` event)
  moved          present at both, canonical body identical and local name
                 identical, while the file identity's path changed on this
                 boundary
  renamed_away   the name-side partner of a name_changed event
  unchanged      present at both, canonical bodies identical (no event)

A body_change that coincides with the file identity changing path carries
`moved: true`. Special events carry `matches`: the number of live
definitions in the whole tree at that commit whose masked span bytes share
the same sha256 (own location included; matches == 1 for a rename/move means
the renamed or moved definition is the only live definition with that
fingerprint, matches == 0 for a deletion means no other definition in the
tree can keep the reference alive). Callees of a definition are the names of
`ast.Call` targets inside it - bare `Name` targets and attribute names of
`Name.attr` / `self.method` targets - intersected with the last naming
segments of the definitions found in the same file version. Unparsable or
non-UTF-8 file versions are marked "unparsable" (no def labels are produced
across them; the last parsable version stays in force) and a
module-qualified name that is mapped twice in one file version produces a
"dup_shadow" marker.

Bookkeeping scope for `matches`: the whole tree includes dead file
lifetimes (paths whose name-status stream ends in a `D` before HEAD, and
earlier lifetimes of HEAD paths). Those lifetimes are tracked silently as
"ghost" identities so their definitions enter and leave `matches` exactly
when their file enters and leaves the tree, but ghosts produce no chains
and no events in the output.
```

### CASE RULE (build_cases.py)

```
All facts come from labels.json files produced by label_history.py (LABEL
RULE in its docstring). Event positions i are indices into the repo's
first-parent lineage (oldest = 0). For one case the replay creates a
veylqora memory that cites one definition - (cite_path, cite_fqn) - at
commit t0, then re-indexes every commit t0+1 .. horizon (window cap
K = 20 commits after t0) and records veylqora's memory states per step.
Dedup key for every case: (repo, cite_path, cite_fqn, t0); a case is
never built twice for the same key. The validity filter for special
events uses the `matches` field of the LABEL RULE: renamed valid iff
matches == 1, moved valid iff matches == 1, deleted valid iff
matches == 0; excluded-with-note counts are reported, never silently
dropped. Type passes run in the fixed order (a), then (b), then specials
in the order renamed, moved, deleted, callee_only.

  a body_change        t0 = c-1 where c is the commit of a body_changed
                       event (moved flag false); cite_fqn = the event's
                       fqn, cite_path = the event's path; horizon = the
                       step at which c occurs; expect flagged
                       (suspected_stale at or before step c-t0).
  b unchanged_neighbours_change
                       anchor at a quiet gap of one chain (no event of
                       the chain between two consecutive events);
                       t0 = m1-1 where m1 is the first file-change bound
                       of the same file identity inside the gap; the
                       window must contain at least one more file-change
                       bound m2 <= t0+K; horizon = min(K, m2-t0)
                       (deviation fix 2026-10-24: first coded as
                       min(t0+K, m2) - an absolute index - while every
                       other case type stores step offsets and replay
                       and scorer consume offsets; case selection
                       unaffected; see the summary doc);
                       expect never flagged (the cited definition's
                       span bytes do not change in the window).
  c renamed            name_changed event with matches == 1; cite_fqn =
                       event old_fqn, cite_path = event old_path;
                       t0 = c-1; horizon = c; expect re-anchored and
                       still active (end state active, never flagged).
                       The renamed_away partner event of the same pair
                       (matches == 1) yields the symmetric case citing
                       the old name.
  d moved              moved event with matches == 1; cite_fqn = event
                       old_fqn, cite_path = event old_path; t0 = c-1;
                       horizon = c; expect re-anchored and still active
                       (end state active, never flagged).
  e deleted            deleted event with matches == 0; cite_fqn and
                       cite_path straight from the event (it carries the
                       removed name and its path); t0 = c-1; horizon = c;
                       expect evidence_missing (flagged).
  f callee_only_changed
                       anchor at a chain whose callees include the local
                       name of a same-file sibling chain that has a
                       body_changed event at commit c2 inside the
                       anchor's quiet gap; t0 = max(prev event of the
                       anchor, c2-5) and < c2; horizon = c2; expect
                       never flagged (only the callee's body changes).

  Relaxation for (b) when candidates are scarce, level 0 first then 1
  then 2: level 0 requires the quiet gap to span at least 3 commits and
  at least 2 file-change bounds in the window; level 1 requires a gap of
  at least 2 commits (still 2 bounds); level 2 requires a gap of at
  least 1 commit and only 1 bound. Each case records its relax level;
  relaxation ledger is written to the output.

  Caps and quotas: per repo the (a) pass may take at most 10 candidates,
  then (b) fills that repo's core pool up to 20 core cases (a+b;
  hard floor 15 per repo - shortfalls are reported, never invented).
  Anchors are stratified by lineage thirds (0..n/3..2n/3..n) picked
  round-robin, each slot sorted by (event commit, identity, chain).
  Candidates whose t0 belongs to a replay group that already exists for
  the repo are taken greedily before candidates at fresh t0 values, to
  limit the number of full indexes. Specials are global across repos
  with quotas: >=5 renamed, >=3 moved, >=8 deleted, >=8 callee_only;
  specials are taken round-robin across repos.

Seed 20261024 fixes tie-breaking randomness (currently none: all
orderings are total or lexicographic; the seed is recorded anyway).
```

### REPLAY MECHANICS (replay_staleness.py)

```
Find the veylqora binary (arg --vey). Probe it with --help and require the
subcommands init, index, memory with sub-commands add, list, inspect; if
the probe fails, stop before touching any repository. Groups come from
cases.json ("groups": sorted by repo and t0). Each group runs on its own
fresh database file (--db) under the results directory, on the clone under
--repos-root/<repo>:

  1. `git -C <repo> checkout -q -f --detach <sha(t0)>`.
  2. `veylqora init --db <db>`.
  3. `veylqora index <repo> --db <db> --json` (full index at t0).
  4. For every case of the group, in cases.json order:
     `veylqora memory add --root <repo> --type decision --content
     "Study case <case_id>: cites <cite_fqn>" --author staleness-replay
     --evidence <cite_path>:<cite_fqn> --db <db> --json`; the returned
     memory id is stored. A failing add is recorded as an error row and
     the case is excluded from later steps (never invented).
  5. `veylqora memory list --root <repo> --db <db> --json` is recorded as
     initial per-case states.
  6. For step s = 1..group horizon: checkout --detach <sha(t0+s)>,
     `veylqora index <repo> --db <db> --json`, then
     `veylqora memory list --json`; each state map (memory id -> state)
     is recorded into steps.jsonl. At steps equal to a case's driver
     commit offset (`driver.commit - case.t0`) one extra
     `veylqora memory inspect <id> --json` per case is recorded.
  7. The database file is deleted after the group; the clone is left
     detached at the group's t0 commit.

Everything observed is written out: add failures, index errors, parse
fallbacks. The runner makes no judgement calls and computes no scores.
```

### SCORING RULE (score_staleness.py)

```
A case is predicted_positive if veylqora ever reports the state
suspected_stale or evidence_missing for its memory at any replay step
1..min(K, horizon) of its group. superseded/invalidated/archived are
non-positive. Ground truth: type a (body_change) and type e (deleted)
are truth positive; types b (unchanged_neighbours_change), c
(renamed), d (moved), f (callee_only_changed) are truth negative (the
memory must stay active; an active memory that was re-anchored after a
rename or move is a correct non-flag).

CORE POOL = types a + b. PASS RULE: "precision >= 0.85 and recall >=
0.85 on the body-change cases". precision = predicted positives whose
truth is positive, divided by all predicted positives of the core pool;
recall = predicted positives among type-a cases, divided by all
type-a cases. Cases whose `memory add` failed are EXCLUDED from every
denominator and reported separately. The same numbers are recomputed
over 10,000 bootstrap resamples drawn with random.Random(20261024) to
give 95% percentile confidence intervals for precision and recall.

SPECIALS are only tabulated, never merged into PASS RULE numbers: for
renamed and moved the end-of-window state versus expectation "active",
for deleted versus "evidence_missing", for callee_only versus
"active"; deviations are listed per case.

For false positives (type b flagged) and false negatives (type a not
flagged) the scorer re-parses the cited definition's file version at
t0 and at the first observably differing step that veylqora acted on
(byte spans from `git show <sha>:<path>`; helpers imported from
label_history.py) and classifies the deviation into exactly one of
these categories: signature-only, docstring-only, decorator-only,
annotation-only, comment-adjacent, whitespace/blank-line-only, other,
not-found, not-parsable, no-change-observed. The classification names
evidence, not an excuse; every category is reported with the case ids
it covers.
```

## Instrument

* `scripts/staleness_replay/label_history.py`, `build_cases.py`,
  `replay_staleness.py`, `score_staleness.py` — independent ground-truth
  side; nothing imports veylqora.
* Subject tree cloned into the execution sandbox (git 2.53.0, python 3.14.4,
  cmake 4.2.3, g++ 15.2.0, ninja 1.13.2, 4 cores) and built once at study
  start; the same binary (sha256 `e6dbedae78f3c7a427f5d1f167624ed49875c4afe
  78d1e7655b19e9038148506`) served all runs — it was never rebuilt after a
  run, so no subject change can contaminate comparisons.
* Study scripts entered the sandbox as byte-identical copies of the study
  branch work tree at `634c1ef` for every post-run-1 execution; the pre-run-1
  state was `9996df4`. SHA256 of the instrument at 634c1ef:
  * label_history.py `2f4297da39a2ca7d32b9d59155287022c55f0b973bdc0ff54141e1eb48a02df9`
  * build_cases.py `bafb9fa48c35a12389c3704e386003b90c6eafd807284fb05571f2614c7c67b0`
  * replay_staleness.py `5357efe32c521a03736bd794ab60ab8da1563b66b555a8bc741ed3794fa6c549`
  * score_staleness.py `47eaeb3722126c6dc0e77da83b5cde24c49f9c183ac3a0fe8be071c3b75b5097`
* Smoke fixtures (`scripts/staleness_replay` smoke tests exercising rename,
  move, delete and twin-file bookkeeping) were re-verified after every
  instrument change; fixture expectations stayed fixed while running.

## Execution ledger (runs 1–3, all times sandbox UTC)

| # | state | labels | cases | replay | score | verdict |
|---|-------|--------|-------|--------|-------|---------|
| 1 | instrument `9996df4`, binary e6dbedae… | original, full lineages | 200 | `results-full/` | `report-run1.json` | FAIL (precision 0.794) — superseded by instrument fixes, retained as evidence |
| 2 | instrument `634c1ef`, repos **left detached** at truncated first-parent prefixes (1309/1407/1438) | labels-run2/ | 84 | `results-full2/` | `report-run2.json` (regenerated deterministically from preserved raw rows) | PASS — procedure invalid (truncated labels; see incident R2) |
| 3 | instrument `634c1ef`, repos reset to pristine clone heads (lineages 1384/1690/1846) | labels/ | 84 | `results-full3/` | `report-run3.json` | **PASS — the final run** |

Incidents, in order:

* **R1 (instrument bugs found after run 1, all fixed by changing the
  implementation *toward* the pre-registered rule text, never the rule
  text):**
  * type-b `horizon` was coded as an absolute lineage index
    (`min(t0+K, m2)`) while every other type and both consumers use step
    offsets — fixed to `min(K, m2-t0)` (`b949fe8`), docstring amended with a
    dated note;
  * the scorer's AST helpers crashed on ClassDef/list fields — guarded
    (`b949fe8`, `27bf36c`);
  * the labeler's `body_changed` compared masked span fingerprints
    (`ssid+span`), contradicting the LABEL RULE's canonical-body comparison —
    fixed to `ast.dump`-based comparison, and span-only edits (comments,
    whitespace) now update the live fingerprint index but emit no event
    (`634c1ef`); the replay's candidate window edges were aligned to the strict
    rule reading (`<`, not `<=`) in the same commit.
  * Fix evidence: the three run-1 a-FNs (click-0003/0007/0008, all
    comment-only anchors), all 7 run-1 b-FPs and all 4 run-1 f-deviations had
    anchor-own events *at the flag commit* — exactly the class the fixes
    remove. Run-1 raw data is preserved in the sandbox
    (`labels-run1/`, `cases-run1.json`, `cases-bug1.json`,
    `results-full/`, `report-run1.json`).
* **R2 (procedure flaw in run 2):** REPLAY MECHANICS step 7 leaves the clones
  detached (that is by design and needed for incremental indexing), but
  run 2's re-labelling ran against those detached clones, so its lineages were
  truncated prefixes (click 1309, jinja 1407, pygments 1438) instead of full
  first-parent histories. Run 2 is retained as a second evidence run but is
  not the reported run. Verification of the flaw's impact: the pristine
  reset (below) shows 76 of 84 case ids would have been selected at identical
  t0 slots; the four b-cases whose t0 moved (pygments-0055/0056/0058/0059)
  and four f-cases (click-0079/0080/0082/0083, whose two run-2 add-failures
  succeeded at the fresh t0s) shift, and none of the moved t0s drives the
  core confusion (see *Cross-run consistency*).
* **R3 (run-3 hygiene):** before re-labelling, every subject clone was reset
  to its frozen clone head (`git checkout -qf --detach` to click
  `2247b35ea1c47c727d7a06e51fa280e12a863ff6`, jinja
  `5ef70112a1ff19c05324ff889dd30405b1002044`, pygments
  `156d85fdd18d34427fe893527e71da91dcfb0eb5`); working trees verified clean
  and lineage lengths verified 1384/1690/1846 from the freshly written
  labels. Any future re-labelling must apply the same reset, because every
  replay leaves the clones detached.

Run-3 timings (sandbox UTC): labelling click ≈ 42 s, jinja ≈ 48 s,
pygments ≈ 59 s (all ~2.5 min total); case build ≈ 30 s; replay of all 32
groups ≈ 2.8 min; scoring incl. 10,000 bootstrap resamples ≈ 1 min.

## Final run (run 3): construction

* 84 cases built ({a:30, b:30, c:5, d:3, e:8, f:8}), 32 replay groups;
  seed `20261024`; `k` 20.
* Per-repo core: 20/20/20 (a=10, b=10 each), shortfall 0 — every quota
  minimum met (core ≥60 with ≥15 per repo: yes; specials 5 renamed,
  3 moved, 8 deleted, 8 callee_only: yes). The (b) pass used relaxation
  level 0 everywhere (ledger: click 10 free picks, jinja 10 free picks,
  pygments 4 free + 10 stratified).
* Labels: click lineage 1384, 91 tracked .py, 8270 events, 20 ghost
  lifetimes; jinja 1690/60/12598/124; pygments 1846/405/7258/61.
* Excluded-with-note ledger at build time (reported, not dropped):
  `b_window_over_cap` 6373, `name_changed_invalid_matches` 208,
  `renamed_away_invalid_matches` 17, `moved_invalid_matches` 79,
  `deleted_invalid_matches` 153.
* `memory add` failures (recorded per REPLAY MECHANICS step 4, excluded from
  every denominator per SCORING RULE): 11 cases —
  click-0015, click-0062, jinja-0030, jinja-0031, jinja-0032, jinja-0033,
  jinja-0036, jinja-0037, jinja-0038, pygments-0051, pygments-0052 (the
  *.py-file classes are known subject-indexer enumeration limits on
  examples/test paths and some nested FQNs, recorded in full in the private
  artifacts).

## Final run (run 3): core result — PASS

Core pool = type a + type b minus add-failures:

| metric | value |
|--------|-------|
| pool | a 30, b 20 (total 50) |
| true positives | 30 |
| false positives | 4 |
| false negatives | 0 |
| true negatives | 16 |
| precision | 0.8823529411764706 |
| recall | 1.0 |
| precision 95% CI (bootstrap, seed 20261024) | [0.7895, 0.9677] |
| recall 95% CI | [1.0, 1.0] |

**Pre-registered PASS RULE: "precision >= 0.85 and recall >= 0.85 on the
body-change cases" → both thresholds met → pass=true.**

## Final run (run 3): specials

| type | scored n | end-state expected | met | flagged (deviation ids) |
|------|----------|--------------------|-----|--------------------------|
| c renamed | 4 | active | 4/4 | none |
| d moved | 3 | active | 1/3 | **click-0065, click-0066** |
| e deleted | 8 | evidence_missing | 8/8 | all eight flagged as expected — no deviations |
| f callee_only_changed | 8 | active | 8/8 | none |

(c is 5 built, 4 scored: click-0062 add-failed. d is 3 built and 3 scored.)

## Deviation analysis (run 3)

**The four core false positives — click-0014, click-0016, click-0017,
click-0018 (cause category from the scorer: `whitespace/blank-line-only`).**
All four cite defs in click (examples/naval/naval.py `mine`,
tests/test_defaults.py `test_multiple_defaults` and its nested `.cli`,
tests/test_chain.py `test_multicommand_chaining`) anchored at t0=766,
horizon 8. Veylqora flagged them at step 1. The scorer's re-parse at the
flagged step shows the cited definition's canonical AST unchanged — the edit
that co-occurs with the flag is whitespace/blank-line-only. Under the
pre-registered LABEL RULE such an edit emits no `body_changed` event, so the
ground truth (never flagged) holds; the flags are early false alarms driven
by non-semantic edits to the anchor's own file.

**The two moved-type deviations — click-0065, click-0066 (control:
click-0067).** All three type-d cases are memories about definitions in
click/_compat.py, created at t0=766; index 767 is click's first-parent
src/-layout move (`click/_compat.py` → `src/click/_compat.py`). Post-hoc
probe (`d_posthoc3.json`, importing the scorer's helpers, run outside the
pre-registered pipeline): at both endpoints all three cited definitions
(`_get_argv_encoding`, `raw_input`, `_is_jupyter_kernel_output`) exist and
compare canonically identical with zero changed parts
(category `moved-identical-canonical`), each `moved` event carries
`matches == 1` (57 definitions of the file moved uniquely at that commit).
The same commit also carries real `body_changed` events for two *other*
definitions of the same file (`_wrap_io_open`, `open_stream`) — neither is a
cited definition. That is, the two flagged memories saw a byte-identical
definition that merely changed path, exactly the case the LABEL RULE calls a
pure move and the CASE RULE expects to end `active`; one def in the identical
situation (the control click-0067, `_is_jupyter_kernel_output`) *was*
re-anchored and stayed `active`. The two flagged ones end
`evidence_missing` at step 1 instead. Same move, same commit, same canonical
bodies, three different outcomes — the subject tool's move handling is
per-symbol inconsistent. This pattern reproduces exactly what run 2 showed
for these same two cases, independently of the run-2 procedure flaw.

**Everything else behaved as specified:** all 30 a-cases flagged in window
(recall 1.0), all quiet cases stayed quiet, deletes ended `evidence_missing`,
renames re-anchored, and all 8 f-type (`callee_only_changed`) cases stayed
silent.

## Cross-run consistency (runs 2 and 3)

* The regenerated run-2 report from its preserved raw rows is bit-for-bit
  equivalent in the graded sections to the run-3 report: core confusion
  tp=30/fp=4/fn=0/tn=16, precision 0.8823529411764706 with CI
  [0.7895, 0.9677], recall 1.0 with CI [1.0, 1.0], and identical FP cause
  listing (click-0014/0016/0017/0018, all `whitespace/blank-line-only`,
  all flagged at step 1). The 8 shifted t0 slots (4 f-type, 4 b-type listed
  above) did not change any graded outcome; the two f-type add-failures of
  run 2 healed at the fresh t0s, leaving 11 add-failures in both runs'
  core accounting.
* Specials likewise: c 4/4, e 8/8, f clean; d stays click-0065 + click-0066
  flagged with click-0067 clean — the same two deviations in both runs.
* Run 1 (pre-fix instrument) is the documented negative result that motivated
  the fixes: precision 0.794 (7 FPs), recall 0.9 (3 FNs), with the FP/FN
  causes traced to the three instrument bugs above. Its raw evidence is
  retained in full.

## Honesty and deviations ledger (complete)

1. Pre-registered rule text was fixed before the first build/run and was
   never changed; all post-run-1 instrument repairs moved code *toward* it
   and are committed (`b949fe8`, `27bf36c`, `634c1ef`) before any re-run.
2. Run 1 failed the PASS RULE under the instrument of the time; it is
   reported here as such and its raw data is retained, not deleted.
3. Run 2 passed the PASS RULE but is procedurally invalidated by incident R2
   (labels re-derived from truncated detached-clone histories). It is
   retained as evidence; its graded artefact was regenerated after the fact
   (`report-run2.json`) from the preserved raw rows and matches run 3's
   graded numbers exactly.
4. Run 3 is the reported run: pristine clone-head reset verified by lineage
   length and clone-head SHA before labelling; identical binary as runs 1–2;
   instrument identical to run 2.
5. `memory add` failures are enumerated per case and excluded from every
   denominator per the SCORING RULE (11 cases in run 3; 13 in run 2;
   14 type-b add-failures in run 1).
6. Specials deviations are post-hoc analyses by a private probe importing
   `score_staleness.classify_pair`/`version_at` helpers — not extensions of
   the pre-registered scorer; the probe output is part of the private
   artifacts (`d_posthoc3.json`).
7. Two formatting/robustness notes: replay stderr for subject sources is
   redirected to private logs; `report.json` currently holds the run-3
   report (copy of `report-run3.json`).

## Artifacts and privacy

Nothing in this repository is raw per-run data: labels, cases, replay rows
and reports live in the study sandbox (`/home/agent/staleness/`) and were
copied to `C:\AI\study-private\staleness\` (labels per run incl. `labels-run1/`,
`labels-run2/`; `cases-run2.json`, `cases.json` (run 3);
`results-full/`, `results-full2/`, `results-full3/`; `report-run1.json`,
`report-run2.json`, `report-run3.json`, `report.json`;
`d_posthoc3.json`; logs; `manifest.json` tracking commits, script and
binary hashes, clone heads, incident ledger and final results). Merge/PR
requests: none. Branch discipline: commits only on `research/staleness-replay`.

## Reproduction recipe (final run)

```
# in sandbox after resetting the three clones to their frozen heads
python3 label_history.py <repo_dir> labels/<repo>.json        # ×3
python3 build_cases.py --labels labels/click.json labels/jinja.json \
    labels/pygments.json --out cases.json
python3 replay_staleness.py --cases cases.json --repos-root repos \
    --results results-full3 --vey <build>/src/veylqora
python3 score_staleness.py --cases cases.json \
    --steps results-full3/steps.jsonl --cases-rows results-full3/cases.jsonl \
    --repos-root repos --lineages results-full3/lineages \
    --out report-run3.json
```

## Limits

* One binary, one build, three repositories, first-parent lineages only.
* Ground truth is AST-lexical: a semantic change that leaves the canonical
  AST bodies unchanged (identical ASTs with different literals is impossible;
  whitespace/comments/docstring-changes deliberately count as non-body) is
  out of scope by the LABEL RULE.
* The PASS RULE interval for precision [0.79, 0.97] spans the 0.85 threshold:
  50 core cases give a coarse CI — the metric result is positive at
  estimate level and not fully decidable in statistical terms at this pool
  size. Recall's CI is degenerate [1.0, 1.0] at 30/30.
* Add-failure exclusions concentrate on examples/test paths (indexer limit);
  they shrink the graded pool but are the honest accounting of what Veylqora
  could actually accept as evidence.
