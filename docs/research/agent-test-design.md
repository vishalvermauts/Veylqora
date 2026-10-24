# Agent study 2 — real bugs on SWE-bench Verified: experiment design

**Design only. No agent has been run. Pending written approval of this design before any run.**

## 1. Claim tested

> An AI coding agent with access to Veylqora's code-search, memory and context
> tools fixes a strictly larger share of real repository bugs than the identical
> agent without them.

"Fixes" is operationalised as the graded SWE-bench verdict on the agent's patch
(§7); "strictly larger share" is operationalised by the pass rule fixed in §10.
Every other section makes that one sentence testable and honest. Nothing in this
document is a result.

## 2. Why this design: contrast with the earlier study

The M3.6b study (`study/README.md`) ran ten seeded one-line bugs in `click` and
ten in `pygments`, one run per cell, with tool hints appended to the prompt for
its B/C/D setups. Its limits are the reasons for this design:

| M3.6b limit | What this design changes |
|---|---|
| Seeded one-line bugs, not representative of real defects | Real, human-validated bugs (SWE-bench Verified) |
| 10 tasks, 1 run per cell | 40 tasks × 3 trials per arm, plus a 60-run pilot |
| Tool hints in the prompt (asymmetric prompts) | Byte-identical prompts; only the exposed toolset differs |
| Two small libraries | Many repositories, SWE-bench-sized codebases |
| Differences indistinguishable from chance | Pre-committed pass rule + cluster-bootstrap interval |

## 3. Task source and selection rule

### 3.1 Sources

- **Primary:** SWE-bench Verified (`princeton-nlp/SWE-bench_Verified`, 500
  human-validated tasks). These are real bugs: each task is a genuine issue in a
  public repository with the repository's own tests as ground truth. This is the
  explicit contrast with M3.6b's seeded bugs.
- **Optional freshness holdout:** SWE-bench Live. If the primary wave completes
  under budget, a second wave on Live tasks (newer than the model's training
  data) measures the contrast without training-data exposure. Its results are
  reported separately and never merged with the primary wave.

### 3.2 Selection rule (fixed before any run, executed in this order)

1. **Universe and order.** All 500 Verified tasks, sorted by `instance_id`
   ascending. Selection is deterministic; no step depends on agent results.
2. **Eligibility, checked per task and logged (private log):**
   - non-empty `FAIL_TO_PASS`;
   - repository language is Python — Veylqora indexes Python/JavaScript/TypeScript
     (`README.md`), and a repo the MCP tools cannot index would make arm B a
     shell with no substance. Non-indexable repos are excluded and the exclusion
     is recorded;
   - the harness eval image builds, and a per-repo spot check holds: the gold
     patch, applied in the private grading container, flips every FAIL_TO_PASS
     test from failing to passing. Done once per repository, kept private.
3. **Symptom authoring** for each eligible task (§4.1), then the
   **blind-localisation band** (§4.3). Keep tasks with band rate in [0.20, 0.70].
4. **Stratification.** At most 2 tasks per repository, walking the ascending
   `instance_id` order; a task whose repository already has 2 accepted tasks is
   skipped and the skip is logged. 40 accepted tasks therefore span at least 20
   repositories, which keeps the per-task clustering of §9 meaningful.
5. **Freeze.** The first 40 accepted tasks in that order are the frozen task
   list. The pilot uses its first 10.
6. **Pre-registration commit.** The frozen list — instance IDs, symptom texts,
   band numbers and selection metadata (repository, language, eligibility
   outcome) — is committed to the repository as an IDs-and-symptom-only file
   **before the first run** (§14). Gold patches, FAIL_TO_PASS/PASS_TO_PASS test
   identifiers and grading metadata never enter the repository: IDs alone are
   not answers, test names and patches are both.

No task may be added, dropped or swapped after freezing. If the frozen list must
change, the change is a documented protocol violation reported with the results.

## 4. Symptom texts and the blind-localisation band

The agent is never shown failing tests or test names. Its only diagnostic input
is the symptom text, so the text's quality decides whether the tasks can test
localisation ability at all. The band measures that rather than assuming it.

### 4.1 Symptom authoring

- For each eligible task the experimenters run the FAIL_TO_PASS tests once at
  `base_commit` in the private grading container to obtain the pre-fix failure
  output (kept private).
- From that failure output the experimenters write the symptom text **once**, in
  their own words: what misbehaves as visible in the failure. A leak checklist is
  applied to every text: no test file names, no test function names, no source
  paths, no fix hints.
- The same text is given to arm A, arm B and the blind solver: the per-task
  prompt is byte-identical across arms (§5).
- Symptom texts are committed as pre-registration together with the IDs. They
  are task inputs, not answers; the band bounds how much location they leak.

### 4.2 Blind solver

A blind solver is the same model (`gemini-2.5-flash`) through the same gateway,
with **no tools and no repository access**. Its input bundle: repository name,
primary language, symptom text. Its output is forced to a single guess as JSON:
`{"file": ..., "function": ...}`.

- 10 independent samples per candidate task at the study's generation settings.
- The band is defined on exactly this input bundle; the bundle is part of the
  pre-registration.

### 4.3 Scoring and acceptance band

- **Hit (per sample):** the predicted repository-relative file path (normalised,
  `a/`-`b/` prefixes stripped) **exactly equals** a file modified by the gold
  patch. Exact file match decides hits.
- **Tie-break by function:** whether the predicted qualified function exactly
  matches a gold-patch-modified function is recorded per sample as a secondary
  statistic and is the tie-break when the pool is truncated to the stratification
  cap and the final 40: candidates rank by (repository cap, in-band position,
  function agreement, `instance_id`).
- **Acceptance band 20%–70%:** a task is kept iff its file-level hit rate
  r satisfies **0.20 ≤ r ≤ 0.70** inclusive. With 10 samples the attainable
  in-band rates are 0.2, 0.3, 0.4, 0.5, 0.6, 0.7.
  - r > 0.70: the symptom alone already localises the bug — the task cannot show
    a benefit from search tools (too easy).
  - r < 0.20: the symptom text is insufficient (symptom-insufficient; agent runs
    on it would measure guesswork, not tool value).
- Band rates for every band-checked candidate are part of the pre-registration
  file, including rejected tasks and their rates.

## 5. Arms

| | Arm A | Arm B |
|---|---|---|
| Tools | Built-in agent-CLI tools only: file read and edit. No shell, no test execution. | Built-in tools **plus** the Veylqora MCP server: `mcp__veylqora__get_project_status`, `search_code`, `inspect_symbol`, `find_dependencies`, `search_memories`, `record_memory`, `inspect_evidence`, `check_memory_validity`, `get_task_context` (tool list as exposed by `mcp/veylqora_mcp/server.py`). |

The arms are identical except for the exposed toolset:

| Variable | Both arms |
|---|---|
| Model | `gemini-2.5-flash` through the existing LiteLLM gateway |
| System + task prompt | byte-for-byte identical (M3.6b's per-setup tool hints are **dropped**: no instruction anywhere tells the agent to use Veylqora beyond the MCP tool descriptions, so the design tests the tools' value when merely available, not the hint) |
| Generation settings, incl. temperature | identical, logged in every run manifest |
| Working copy | fresh checkout of the task repository at `base_commit`, per run |
| Container image | same image both arms |
| Time budget | 45 min hard cap per run (above M3.6b's 15 min; repos are larger) |
| Retry policy | identical (§6) |
| Test access | none (§7) |

**Arm-B state.** Before each arm-B run, the task repository at `base_commit` is
indexed into a **fresh** Veylqora database (own daemon on its own loopback port,
single writer per database per `README.md`). Memory starts empty. The agent may
`record_memory` and re-read it **within the same run only**. Cross-trial and
cross-task carryover is deliberately disabled so the three trials of a task stay
exchangeable; a shared per-task database would let trial 2 inherit trial 1's
memories. The cost of this choice is stated in §8.

## 6. Trials, model, run policy

- **Trials:** 3 per task per arm → 6 runs per task. Full study: **40 tasks × 6 =
  240 runs** (chosen task count: 40).
- **Order:** the 6 runs of a task are interleaved in a deterministic permutation
  seeded per task with fixed seed `20261024`, so gateway condition drift cannot
  align with one arm.
- **Served model** is read from the response stream every run (v3 discipline).
- **Retries:** infra/gateway failures are retried up to 3 times, each retry on a
  fresh copy (fresh checkout, fresh database). A run that still fails is recorded
  as an **infrastructure error and is not scored**; scoring denominators contain
  scored runs only, and infra-error counts and positions are reported per arm.
- **Hygiene:** each run's temporary folder is removed, including on abort.
- The agent cannot run commands or tests in either arm. Honest consequence:
  absolute pass rates will be far below public SWE-bench agent results. This
  design measures the marginal value of the toolset under a fixed no-test-access
  policy that is identical for both arms — not maximal bug-fixing performance.

## 7. Grading

- The agent's patch is `git diff base_commit` of its working copy.
- Grading runs in a **clean SWE-bench harness container per run**, using the
  repository's own eval image, executing the task's FAIL_TO_PASS and PASS_TO_PASS
  tests from the SWE-bench metadata.
- **A run passes only if all FAIL_TO_PASS tests become passing and all
  PASS_TO_PASS tests remain passing.**
- The agent has no access to the tests at any point: no test execution in the
  runner sandbox, no test names in any prompt, grading metadata only in the
  private out-of-repo bundle.
- **Test-file mutation:** any patch touching the repository's test paths (per its
  SWE-bench metadata) disqualifies the run — recorded as `test_mutation`,
  reported, **not scored**.
- Grading infrastructure failure → regrade on a fresh container up to 3 times;
  still failing → recorded as an infrastructure error, run not scored.

## 8. Expected contribution of the tools, and what is not claimed

Grounded in what is already measured, not hoped:

- **Expected:** Veylqora search places the real fix's file first for ~40% of 113
  held-out real bug reports vs 23.0% for plain text search (`README.md`). On
  SWE-bench-sized repositories, where built-in tools must grep or guess, search
  is the plausible source of a localisation advantage — which this design converts
  into a fix-rate comparison.
- **Not claimed in this design:**
  - **Memory accuracy on real repos.** ADR-0014's measured precision 1.00 /
    recall 1.00 for staleness detection come from a ten-scenario hand dataset
    (`tests/integration/test_staleness.cpp`) only.
  - **Accumulated memory.** Every arm-B run starts with an empty memory (§5), so
    memory tools can contribute only within a single session (record then
    retrieve). The value of memory accumulated and curated across sessions is out
    of scope here and remains unmeasured.
  - **Per-tool attribution.** Arm B exposes the toolset as a whole; the design
    cannot say which tool caused any observed difference. Two arms were chosen
    because 40 tasks × 3 trials support two arms, not four, and M3.6b's
    four-setup differences were within chance.
  - **Languages Veylqora cannot index.** Non-Python repositories are excluded
    (§3.2, recorded) for exactly this reason.
  - **Masking.** An observed difference is a toolset effect; the design does not
    claim the tools help every task or every agent.

## 9. Primary metric and statistics

- Binary graded outcome per scored run (§7).
- **pass@1 per (task, arm):** passing runs ÷ scored trials (values 0, ⅓, ⅔, 1).
- **Paired by task:** the per-task difference d = pass@1(B) − pass@1(A).
- **Headline: the mean over the 40 tasks of that paired difference**, in
  percentage points, with a **95% confidence interval from a cluster bootstrap
  over tasks** — clusters are tasks, not runs; 10,000 resamples with replacement;
  fixed seed `20261024`; percentile interval.
- Also reported, not part of the pass rule:
  - per-repository breakdown of pass@1 by arm (descriptive; repositories
    contribute 1–2 tasks each);
  - trial variance: the distribution of outcome spread within (task, arm) cells;
  - infra-error and disqualification counts per arm;
  - per-run Veylqora tool-call counts in arm B, and arm-A time-to-final-diff
    distributions.
- **Power note, pre-committed:** if the per-task difference SD exceeds ≈ 0.35,
  a 40-cluster bootstrap interval is wider than roughly ±11 points and the §10
  rule cannot be met; the correct report is then "not supported". The rule is
  not relaxed after seeing the interval.

## 10. Pass rule (fixed before the first run)

Quoted verbatim and not open to re-wording in any results document:

> "B beats A by at least 15 percentage points with the lower bound of the
> interval above 0. Otherwise the claim is reported as not supported."

No re-analysis, no metric switching, no post-hoc subgroups after results. Any
secondary analysis that was not declared above is labelled exploratory in the
write-up and cannot feed a README claim.

## 11. Stop rules (every stop is reported; nothing is silently recovered)

| Stop | Trigger | Action |
|---|---|---|
| Halt for repair | total infrastructure-failure rate > 15% of runs so far | stop; fix infrastructure; affected runs re-run on fresh copies; repair note published with the results |
| Batch pause | more than 3 consecutive gateway failures | pause the batch; report; resume only once the gateway is stable; the paused run re-runs on a fresh copy |
| Budget cap | $800 in gateway dollars or 150 sandbox-hours reached | stop; report partial results with the cluster-bootstrap interval, clearly labelled partial |
| Selection repair | blind-band reject rate > 50% during selection | stop selection; revise symptom authoring or band procedure; re-band the whole pool and refreeze IDs; at most 2 repair rounds, then report and stop |
| Wiring break | served model ≠ requested model in any run after the pilot | pause immediately; report; affected runs re-run after repair on fresh copies; mismatched runs are classified infra and never scored |
| Cost drift | measured pilot cost outside the ±25% go band | pilot is NO-GO (§12) |

Batching supports these rules: after the 10-task pilot, the full study runs in 4
batches of 10 tasks (60 runs each); sandboxes stop between batches; cumulative
cost and infra-rate are checked at every batch boundary.

## 12. Pilot: 10 tasks × 2 arms × 3 trials = 60 runs

The pilot runs first, on the first 10 tasks of the frozen list. It exists to
check wiring only — **pilot results are never evidence for the claim**, and
pilot runs are separate from the 240 main runs.

Checks — all must be green for GO:

1. **Served model == requested** in all 60 runs (read from the stream).
2. **Arm-B tool usage:** at least 44% of arm-B pilot runs call at least one
   `mcp__veylqora__` tool. The design's reference threshold is "expected ≥ 8 of
   18 arm-B runs", i.e. a 44% floor; scaled to the pilot's 30 arm-B runs the
   gate reads **≥ 14 of 30**.
3. **Grading reproduces SWE-bench verdicts on a spot-check,** per pilot
   repository: grading the untouched `base_commit` reports the FAIL_TO_PASS
   tests failing; grading the gold patch (applied only in the private grading
   container) reports them passing with PASS_TO_PASS intact; plus transcript-vs-
   verdict agreement on 2 sampled pilot runs.
4. **Per-run cost and wall-clock are recorded** for all 60 runs.
5. **No test-file mutations** in any pilot run.

**GO** requires all five checks green **and** the measured pilot cost within
25% of the estimate (estimated pilot: ≈ $77; go band: $58–$96).

**NO-GO** findings block the full run and are reported with a repair list; the
pilot may be re-run after repairs.

## 13. Cost and time estimate, and sandbox plan

### 13.1 Deriving the per-run cost

M3.6b measured ≈ $1.51 for 40 runs on `click`-sized tasks (≈ $0.04/run; short
sessions, tiny files). SWE-bench repositories are one to two orders of magnitude
larger, agents must explore more, and long tool outputs inflate paid input while
agent turns inflate output (the dominant price: $2.50/M tokens). **Assumed
upward multiple: 30–40×, i.e. ≈ $1.15 per arm-A run and ≈ $1.40 per arm-B run**
(arm B slightly higher: more tool calls, more turns). Cost per run is computed
from measured usage at the gateway's published prices (input $0.30 / cache read
$0.03 / output $2.50 per million tokens), as in M3.6b v3.

| Item | Runs / calls | Est. gateway $ | Est. sandbox-hours |
|---|---|---:|---:|
| Blind selection: ~150–250 candidates × 10 samples, no tools | ~2,000 short calls | ≈ $5 | ≈ 2 |
| Harness bring-up: image builds + gold-patch spot checks (~20 repos) | — | $0 | ≈ 15 |
| Per-task fail-log generation at `base_commit` (symptom source) | ~200 × grades | $0 | ≈ 15 |
| **Pilot** (10 tasks × 2 × 3) | 60 | ≈ $77 | ≈ 8 |
| **Full study** (40 tasks × 6 = 240 runs) | 240 | ≈ $306 | ≈ 45 |
| Grading: clean container per run (300 gradings + spot checks) | 300 | $0 | ≈ 28 |
| Veylqora indexing at `base_commit` (120 arm-B runs; measured 2–11 s per ~10³ files, plus daemon start) | — | $0 | ≈ 1 |
| **Total planned** | | **≈ $395** | **≈ 105–115** |
| **Hard caps (stop rules)** | | **$800** | **150** |

Estimates are pre-run guesses with ≈ ±40% uncertainty; the caps and the pilot
cost gate turn bad luck into a reported stop instead of a silent overrun.

### 13.2 Sandbox plan

- **Sandbox R (runner):** the agent CLI (same CLI as M3.6b, version pinned and
  recorded in the run manifest), the gateway client, and the Veylqora daemon +
  MCP server. Each run gets a fresh checkout, a fresh database and its own
  daemon port. Up to 4 agent runs in parallel per batch. The study key reaches
  the CLI through `apiKeyHelper` from a root-only file — the key is not in the
  repository.
- **Sandbox G (grading):** the SWE-bench harness, with **one clean grading
  container per run** using the repository's eval image. Test metadata is
  mounted read-only from the private out-of-repo bundle. Sandbox G also performs
  image builds, gold-patch spot checks and per-task fail-log generation.
- Both sandboxes stop between batches (§11).
- The `veylqora` binary is built Release from a pinned commit; its **sha256 is
  recorded in the pre-registration bundle**.
- **Raw data lives outside the repository:** per-run transcripts, tool-call logs
  and grading logs in a root-only folder in each sandbox, mirrored to a private
  folder outside the repository. Never committed; the repository receives only
  the pre-registration bundle and, later, aggregated results documents.
- Every run records (v3 discipline): served model(s) from the stream, token
  usage and gateway cost, wall-clock, every tool call by name, and the outcome
  class (`pass` / `fail` / `infra` / `test_mutation`).

## 14. Pre-registration and evidence hygiene

- **Committed before the first run:** this document plus the frozen bundle —
  instance IDs, per-task symptom texts, blind-band numbers (kept and rejected),
  and selection metadata. Claim, band, arms, trials, metric, pass rule and stop
  rules live in this file and are committed with it. After freezing, any change
  is a dated addendum; silently rewriting the frozen list is a protocol
  violation reported with the results.
- **Never in the repository:** gold patches, FAIL_TO_PASS/PASS_TO_PASS test
  identifiers, grading metadata, raw transcripts, grading logs, per-run key
  material. IDs alone and symptom texts are task inputs, not answers.
- **README discipline:** no agent-study results are added to `README.md` until
  this study is approved, complete and reviewed. Until then the README's
  "AI agents fix more bugs with Veylqora ❌ not proven" line remains exactly as
  it is.
- The earlier study's limits — 10 tasks, 1 run per cell, seeded one-line bugs,
  hint asymmetry (`study/README.md`) — are cited as the reasons for this design
  (§2, §5), not buried.

## 15. Limitations, declared in advance

- **Training-data exposure.** SWE-bench Verified is public; the model may have
  memorised fixes. Exposure is arm-independent, so the paired contrast stays
  internally valid, but absolute pass rates and the headroom for a 15-point gap
  may be inflated. SWE-bench Live exists as the counter-measure (§3.1).
- **Two arms, one bundled toolset** — no per-tool attribution (§8).
- **No-test-access policy** lowers absolute performance vs conventional
  SWE-bench agent setups (§6). Absolute levels are not the claim; the paired
  gap is.
- **40 tasks** give at best borderline power for the 15-point rule (§9).
- **Cold memory every run** (§5): memory is tested as an in-session tool only.

## 16. Checklist before the first run

- [ ] This design approved in writing (approval recorded with the commits).
- [ ] Selection executed exactly as §3: eligibility log, symptom texts written
      and leak-checked, blind solver run, band numbers computed.
- [ ] Frozen bundle committed to the repository: IDs only + symptom texts +
      band table + selection metadata. No patches, no test names anywhere.
- [ ] Fixed seeds recorded: selection ordering (deterministic) and bootstrap
      seed `20261024`; generation settings, agent-CLI version and harness
      version recorded in the bundle.
- [ ] `veylqora` binary built Release from the pinned commit; sha256 recorded
      in the bundle.
- [ ] Budget caps ($800 gateway dollars, 150 sandbox-hours) acknowledged by
      whoever funds the runs.
- [ ] Sandbox R up: gateway client, daemon-per-run port scheme, `apiKeyHelper`
      key file in place (root-only, not in the repository).
- [ ] Sandbox G up: eval images build; gold-patch spot checks pass for all
      pilot repositories; fail-log generation works.
- [ ] Private storage created outside the repository for raw transcripts and
      grading logs.
- [ ] Pilot run (60 runs) → all five §12 checks green and measured cost within
      $58–$96 → GO recorded in writing → only then does the full study start.
