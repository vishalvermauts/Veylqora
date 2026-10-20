# ADR-0015 — Hybrid retrieval: lexical, vector and graph, fused by reciprocal rank

- **Status:** accepted; thresholds fixed before the labelled queries were run
- **Date:** 2026-10-20

## Context

M2.1 needs retrieval that finds code for a task in words, not only by exact
name (plan §2.1). The plan asks for three signals (exact lexical, semantic
vector, graph), metadata filters, deterministic ranking, and measured recall.

## Decisions

1. **Lexical.** Identifiers are split on non-alphanumerics and at camelCase
   boundaries, lower-cased, and kept if at least two characters long. Each
   symbol's name, qualified name and body tokens are stored in a `token`
   table, indexed by project and token. A query scores each symbol by the number
   of distinct query tokens it contains.

2. **Vector.** Each symbol gets a 256-dimension vector: tokens are hashed
   (FNV-1a) to a dimension and a sign, weighted by where they occur (name 3,
   qualified name 2, body 1), then normalised. Query and symbol vectors use the
   same function, so similarity is a dot product.

   **This is not a learned semantic model.** It captures shared identifiers and
   near-synonyms only where they share tokens. A real embedding model needs a
   pinned model file and an inference runtime; both are a separate decision and
   are not made here.

3. **Search is exact.** Vector retrieval scores every symbol in the project. ANN
   indexing (USearch, ADR-0002) is deferred to M2.2, where index maintenance is
   designed; the exact search is the ground truth an ANN index must match.

4. **Graph.** The top symbols from the first two signals seed a one-hop walk
   over resolved and heuristic edges. A symbol reached from several seeds ranks
   higher.

5. **Fusion.** Reciprocal rank fusion, `score = Σ weight / (60 + rank)`, with
   weights lexical 1.0, vector 1.0, graph 0.5. Ties are broken by symbol id, so
   a fixed input and configuration always give the same order.

6. **Bounds.** At most 100 results. A query of at most 512 characters. A query
   with no searchable token is refused rather than returning everything.

7. **Isolation.** Every row and query is filtered by project.

## Thresholds (fixed before the labelled queries were run)

On the labelled queries in `tests/integration/test_retrieval.cpp`, mean
recall@10 over the hybrid mode must be ≥ 0.80. Lexical and vector modes are
reported alongside for comparison and are not gated.

## Measured ground truth

Exact vector search is the reference: an ANN index added later must reproduce
its top-10 at recall ≥ 0.90 (plan §2.1) before it replaces exact search.

## Results

Eight labelled queries on the 10-file Python fixture (`tests/fixtures/python`):
mean recall@10 is 1.00 for lexical, vector and hybrid. The gate of 0.80 passes.

**This does not show that hybrid retrieval is better.** Every query has a
distinctive identifier, so lexical matching alone already finds the answer.
The labelled set cannot separate the three modes. A set that can is needed
before any claim about the vector or graph signals; it belongs with the offline
evaluation in M3.6a, drawn from a larger corpus with queries written without
looking at the results.

The vector signal is deterministic token hashing (see Decisions 2). It is not
a semantic model, so it will not find code that shares no identifiers with the
query. That gap is the reason a learned model is still needed.
