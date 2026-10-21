# Benchmarks (M3.2)

Latency, indexing time and memory for the daemon, measured through its HTTP API
on generated corpora of 200, 1,000 and 5,000 Python files.

## Reproduce

The `bench` CI job builds a Release binary and runs the script in the cloud
sandbox, which is the reference environment for these numbers:

```bash
scripts/ci.sh bench
```

To run it on your own machine:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVEYLQORA_BUILD_TESTS=OFF
cmake --build build --target veylqora
VEYLQORA_BIN=build/src/veylqora python3 scripts/bench.py --out docs/benchmarks/mine
```

## What is measured

- **Cold index time:** one full index of the corpus into an empty database.
- **Incremental index time:** after 1% of the files change.
- **Retrieval latency** (`/api/v1/retrieve`, k = 10) in lexical, vector and
  hybrid modes: p50, p95 and p99 over 200 queries after 20 warm-up queries.
- **Name search latency** (`/api/v1/search`): p50, p95 and p99.
- **Daemon resident memory and CPU time**, read from `/proc` after the run.

## Manifest

Every results file carries a manifest: platform, CPU model, core count, compiler,
the binary's SHA-256, the query set, warm-up and measured counts, and a SHA-256
of each generated corpus. A result without its manifest is not valid evidence.

The corpus is generated from a fixed seed (7), so every run of the same size
sees the same files. Each run uses a freshly generated copy, because the
incremental step edits files.

## Results (3 runs per size, reference sandbox)

Figures are medians of the 200 measured queries per run. CV is the coefficient
of variation of the median across the 3 runs.

| Files | Mode | p50 (ms) | max CV | p95 range (ms) |
|---|---|---|---|---|
| 200 | lexical | 1.9–2.2 | 8.8% | 2.7–3.1 |
| 200 | vector | 3.2–3.6 | 6.1% | 3.7–4.1 |
| 200 | hybrid | 3.5–3.8 | 5.2% | 4.3–4.7 |
| 1,000 | lexical | 9.2–10.0 | 4.3% | 13.1–14.2 |
| 1,000 | vector | 14.9–15.3 | 1.5% | 16.9–17.2 |
| 1,000 | hybrid | 16.0–16.4 | 1.2% | 20.6–21.4 |
| 5,000 | lexical | 63–69 | 4.1% | 90–96 |
| 5,000 | vector | 100–112 | 5.7% | 114–129 |
| 5,000 | hybrid | 100–118 | 9.3% | 130–147 |

Cold index time: 0.3 s (200 files), 1.7–1.8 s (1,000 files), 15.6–16.4 s
(5,000 files). Daemon resident memory: about 20 MB, 44 MB and 178 MB at the
three sizes.

## Gates

- Variance across runs at or below 10% for every size and mode: met (largest
  9.3%).
- Hybrid p95 at or below 200 ms (M2.1): met at every size (largest 147 ms).
- Results committed with their manifest: met.

## Limitations

- **Shared cloud VM.** The reference environment is a shared sandbox. Variance
  of up to 9% is measured against that noise, and a dedicated machine would
  likely show less.
- **Generated code.** The corpus is simple: five short functions per file with
  synthetic calls. Real code has more tokens per definition and more name
  collisions, so real latencies will differ.
- **Three runs.** Three runs give a rough picture of variance. More runs would
  tighten the estimates.
- **Single client.** Queries come from one sequential client. Concurrent load
  is not measured.
- **p99 is not gated.** p99 from 200 samples is noisy; it is reported in the
  results file but not gated.
