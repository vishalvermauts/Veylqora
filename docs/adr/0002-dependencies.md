# ADR-0002 — Dependency acquisition and vector index

- **Status:** accepted
- **Date:** 2026-10-20

## Context

The directive left two choices open: how dependencies are acquired
(vcpkg vs. FetchContent) and which vector index is used (USearch vs. FAISS,
"selected through benchmarks"). Both were blocking the first build.

Deciding them late is expensive: the dependency mechanism shapes CI, the
Dockerfile and every developer's first five minutes, and the index choice
shapes the storage layer and the incremental-maintenance design.

## Decision

**Dependencies: CMake FetchContent with immutable pins.**

- vcpkg adds a multi-hundred-megabyte bootstrap and a long first build to every
  environment, including each billable cloud sandbox session.
- The dependency set is small (SQLite, a digest header, a test framework,
  tree-sitter, one vector library) and all of it builds cleanly with
  FetchContent.
- Every dependency is pinned to a tag or commit SHA, declared `SYSTEM` so its
  warnings do not reach our `-Werror` build.
- SQLite is taken from the system package where available, for distro security
  updates; the amalgamation fallback is added only if a supported platform
  lacks it.

**Vector index: USearch.**

- Header-only, Apache-2.0, no BLAS dependency. FAISS on MSVC needs a BLAS stack
  and is a known-difficult Windows build, so a "fair bake-off" across our two
  supported platforms was largely foreclosed before it started.
- USearch supports incremental add and remove plus serialisation, which is what
  a single-node incremental workload needs. FAISS's IVF families do not support
  efficient deletion at all.
- Spending a module on a bake-off whose outcome is predictable costs more than
  deciding now and recording what would reverse the decision.

**Reversal criteria.** Revisit FAISS if, on the reference platform, any holds:
recall@10 below 0.90 at a p95 query latency budget of 200 ms on a corpus of
≥ 1M vectors; index build time more than 3× a FAISS baseline; or a correctness
defect in USearch delete/compaction that upstream does not fix within one
release.

## Consequences

- First build needs network access to GitHub; the sandbox network allowlist
  must include it.
- No binary caching across sandbox sessions, so a `ccache` volume or a prebuilt
  base image becomes worthwhile once dependency build time exceeds ~2 minutes.
- Module 2.1's work is implementing the index, not choosing it.

## Addendum — spike S3 result (2026-10-20)

Measured with `spikes/s3_usearch` (USearch v2.26.5, cosine, f32, 128 dims,
20k vectors, 50-cluster Gaussian mixture, 200 queries, k=10, exact brute-force
ground truth). Raw output: `docs/research/s3-usearch-deletion.txt`.

| HNSW (M / expansion_add / expansion_search) | Deleted | recall@10 (tombstoned) | recall@10 (rebuilt) | search µs |
|---|---|---|---|---|
| 16 / 128 / 64 | 0% | 0.484 | 0.518 | 311 |
| 16 / 128 / 64 | 50% | 0.644 | 0.647 | 601 |
| 32 / 256 / 256 | 0% | 0.978 | 0.983 | 1552 |
| 32 / 256 / 256 | 25% | 0.984 | 0.990 | 1948 |
| 32 / 256 / 256 | 50% | 0.995 | 0.997 | 2315 |

Findings:

1. **Default-like HNSW settings fail the 0.90 gate** on this corpus, even with
   no deletions. The index must be built with M=32, expansion_add=256,
   expansion_search=256 (or measured higher settings) — this is now a pinned
   requirement for Module 2.1, not a default.
2. **Tombstoned search matches a rebuild** within ±0.006 recall at every
   deletion level tested. Compaction is therefore needed for storage reclaim
   and index hygiene, not for recall, on this workload.
3. **No deleted key was ever returned** (`leaked=0` in every row).
4. **Search latency rises with tombstones** (1552 → 2315 µs/query, 0 → 50%).
   Compaction threshold should be set by latency, not only by tombstone ratio.
5. Deletion is cheap: ≤2 ms for 10k removals.

Limits of this spike (recorded so they are not forgotten):

- Random deletion only. Deleting high-connectivity hub vectors, or deleting in
  clustered batches, may degrade recall more; not tested.
- Synthetic corpus. Real code-chunk embeddings have different geometry and must
  be re-measured in M2.1 before the numbers are used as a gate.
- Single-threaded build and query (`threads = 1`) for reproducibility.

**Decision:** ADR-0002 stands. Reversal criteria are unchanged. Module 2.1 pins
M=32 / expansion 256 and re-runs this spike on real chunks before the gate is
set.
