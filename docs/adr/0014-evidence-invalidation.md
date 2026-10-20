# ADR-0014 — Evidence-aware invalidation (M2.3)

- **Status:** accepted; thresholds fixed before the labelled dataset was run
- **Date:** 2026-10-20

## Context

A memory cites the symbols it is based on (ADR-0011). When the code changes,
the memory must be re-examined, but it must not be declared false on the strength
of a file edit (plan §2.3). The system needs a rule that decides, for each
memory, which of three things happened: nothing relevant changed, the cited code
changed, or the cited code disappeared.

## Decisions

1. **A cited symbol changed** when its current body fingerprint differs from the
   fingerprint stored with the evidence. Comments and whitespace do not count,
   because the fingerprint normalises them (ADR-0003). A changed signature does
   count, because the signature is part of the definition's text.

2. **A cited symbol that is gone is re-anchored when its exact body still exists
   elsewhere** in the project, and it is reported missing otherwise. A rename or
   a move therefore keeps the memory active. A rename that also edits the body is
   a missing symbol, which is the documented limitation of ADR-0003.

3. **Transitions the system may make on its own:**
   - `active → suspected_stale` when a cited symbol changed;
   - `active → evidence_missing` when a cited symbol cannot be found;
   - `suspected_stale → active` and `evidence_missing → active` when every cited
     symbol matches its stored fingerprint again (a revert restores the memory).
   Nothing else is automatic. Reaching `invalidated` needs a recorded decision,
   and so does accepting a changed symbol: `accept_memory` refreshes the stored
   fingerprints and records who accepted and why.

4. **Previous evidence is preserved.** A detected change leaves the original
   fingerprint in place; only an explicit accept replaces it.

5. **Revalidation runs after every completed indexing run that changed something**,
   so the state stays current without a separate step.

## Thresholds (fixed before the dataset was run)

On the labelled dataset in `tests/integration/test_staleness.cpp`, measured over
memories whose true state is "needs attention" (flagged) versus "unaffected":

- detection precision ≥ 0.80
- detection recall ≥ 0.70

These are the plan's targets (§2.3). They are not adjusted after seeing results.

## Not covered

- **Conflicting memories** (two memories that contradict each other) are not
  detected. Nothing in the data model says what contradicts what; that needs
  reviewed relations, not inference. Recorded as a gap, not a claimed capability.
- **Semantic change** (same behaviour, different body) is reported as changed.
  That is deliberately conservative: a false alarm costs a review, a missed
  change costs a wrong memory.
- **Cost.** Revalidation looks up symbols by fingerprint with a scan. That is
  acceptable at this scale and is measured, not assumed, in M3.2.

## Results (dataset in `tests/integration/test_staleness.cpp`)

Ten scenarios, three labelled as needing attention. The thresholds above were
written before the first run and were not changed.

First run, before two defects were fixed:

- precision 0.60, recall 1.00 (tp 3, fp 2, tn 5). The two false positives were
  a comment added inside a cited body (the fingerprint counted it) and a
  superseded memory whose cited code later changed. The second was a defect in
  the test's own definition of "flagged", which counted any non-active state.
- Fixes: comments are removed from the fingerprinted text, and a comment that
  fills its own line takes the line with it. "Flagged" now means suspected stale
  or evidence missing. A superseded memory is a decision, not a finding.

Final run: all ten scenarios match their labels (precision 1.00, recall 1.00).
This is a small, hand-labelled dataset. It shows that the rules work on the cases
it covers. It does not establish accuracy on real repositories; that is a
measurement for M3.6a.
