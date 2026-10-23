# Veylqora

**Veylqora is a local code-intelligence service with persistent memory for AI coding agents.**

It indexes a repository's code (symbols, calls, imports, inheritance), stores what
an agent has learned about that code as structured memories, and keeps each
memory tied to the code it was based on. When that code later changes, the
memories that depend on it are the ones in question. The design goal is to tell
an agent *which* stored knowledge may be stale, instead of having it trust the
notes blindly or ignore them.

**Status (October 2026): Phases 1 and 2 are certified, and the core system works end to end.** Phase 3 is partly done. In a controlled study on 10 seeded bugs, an AI coding agent connected to Veylqora, used its tools in 30 runs (49 calls), and every run completed cleanly with no timeouts or edited tests. The study did not yet show that the tools raise the agent's pass rate: 5/10 without the tools, 6/10 with search and memory, 4/10 with search alone and 3/10 with the context package. Ten bugs with one run each is too small to tell these apart, so a larger study is planned. See [Current implementation](#current-implementation) and the [agent study notes](study/README.md).

---

## The problem

AI coding agents lose context between sessions. In practice this shows up as:

- **Re-investigation.** The agent re-reads the same files and re-discovers the same
  facts every session, spending tokens and time on work it already did.
- **Stale notes.** A note saying "the token check is in `auth.py`" stays in the
  notes after the function is renamed, moved, or rewritten. The agent acts on it.
- **Irrelevant retrieval.** Searching by text returns the word "token" in twenty
  places; the agent cannot tell which one matters for the task.
- **No provenance.** Nothing records *why* a decision was made, *who* made it, or
  *what code it assumed*.

Veylqora addresses each of these with a persistent, queryable model of the code
and of the agent's knowledge, linked together.

## How it helps

| Need | What Veylqora provides |
|---|---|
| Find the code that matters | Exact symbol lookup; hybrid retrieval (IDF-weighted lexical, hashed-token vector, and one-hop graph fusion); graph queries for who calls or depends on a name |
| Understand relationships | Calls, imports, inheritance and containment, each with a confidence level |
| Keep knowledge across sessions | Memories with revisions, state, author and reason, stored in SQLite |
| Know when knowledge is stale | Memories cite the symbols they rely on, with a fingerprint of their body at the time. Re-indexing automatically re-anchors renamed/moved definitions and marks modified or deleted evidence (`suspected_stale` or `evidence_missing`) |
| Don't trust guesses | Every graph edge is marked `resolved`, `heuristic` (a guess) or `unresolved`. The tool never presents a guess as fact |
| Audit and recover | Every memory change is recorded. Export and import are deterministic and byte-identical on round trip |
| Stay local and private | No network calls for indexing, graph or memory. Nothing leaves the machine |

## How it works

```mermaid
flowchart LR
    src[("Source tree")] --> ing["Ingestion: git ls-files honours .gitignore; skips symlinks, binary, invalid UTF-8"]
    ing --> parse["tree-sitter parse, one file at a time"]
    parse --> facts["Facts: calls, imports, bases, name bindings"]
    facts --> db[("SQLite in WAL mode, one writer")]
    db --> res["Resolver: whole-project pass"]
    res --> edges["Edges: resolved, heuristic or unresolved"]
    edges --> db
    user(["Agent or user"]) --> api["CLI or HTTP API, loopback only"]
    api --> db
    db --> mem["Memories: revisions, evidence, events"]
```

1. **Ingestion.** Inside a git work tree, the file list comes from `git ls-files`, so
   `.gitignore` rules apply exactly as git applies them. Symlinks are never followed.
   Binary files, non-UTF-8 files and files over 1 MiB are skipped with a warning.
2. **Parsing.** Each Python, JavaScript and TypeScript file is parsed with tree-sitter.
   The parser records symbols (modules, classes, functions, methods, with dotted names
   and full decorated spans), call sites, inheritance, and what each import binds to.
   A file with syntax errors is still indexed for its valid parts.
3. **Resolution.** After a run that changed something, a whole-project pass turns
   the recorded facts into edges. A name bound by an import in the same file is
   `resolved`. A name defined in the same file is `resolved`. A method called on an
   unknown receiver is `heuristic`, linked to at most five candidates. Anything else
   is `unresolved`. Package re-exports are followed for a bounded number of hops.
4. **Memory.** A memory has a type, content, author, scope and state. It can cite
   indexed symbols as evidence; a citation that names nothing indexed is rejected
   and the whole memory is not created. Updates use optimistic concurrency: a writer
   names the revision it read, and a stale write is refused with a conflict rather
   than silently overwriting.
5. **State machine.** States are `active`, `suspected_stale`, `superseded`,
   `invalidated`, `evidence_missing` and `archived`. Only the transitions in the
   table are allowed, and each needs a reason. A code change can raise a question
   (`active → suspected_stale`); answering it is a separate, recorded decision.

```mermaid
stateDiagram-v2
    [*] --> active
    active --> suspected_stale : code changed
    active --> evidence_missing : cited symbol deleted
    active --> superseded : replaced by a newer memory
    suspected_stale --> active : revalidated
    suspected_stale --> invalidated : proven wrong
    suspected_stale --> superseded : replaced
    evidence_missing --> active : re-anchored
    evidence_missing --> invalidated : proven wrong
    active --> archived
    suspected_stale --> archived
    evidence_missing --> archived
    superseded --> archived
    invalidated --> archived
    archived --> [*]
```

## Test results

Last full CI run on the code in this repository (cloud sandbox, Ubuntu 26.04,
gcc 15.2 and clang 21.1):

| Job | Result |
|---|---|
| clang-format check | PASS |
| gcc 15 build, `-Werror`, all tests | PASS — 105 / 105 |
| clang 21 build, `-Werror`, all tests | PASS — 105 / 105 |
| AddressSanitizer + UndefinedBehaviorSanitizer, all tests | PASS — 105 / 105 |
| MCP server end-to-end (`pytest mcp/tests`) | PASS — 20 / 20 |
| Offline evaluation (`scripts/evaluate.py`) | PASS — hybrid recall@10 `1.000`, MRR `0.938`, stale precision `1.000`, recall `1.000` |
| End-to-end packaging & backup/restore (`scripts/e2e.sh`) | PASS |
| Web dashboard build + Vitest + Playwright browser suite | PASS — 9 unit + 6 browser tests |
| Outbound event adapters (`adapters/tests`) | PASS — 9 / 9 |
| Certification records verify | PASS (records for older commits are reported as stale warnings) |

The 105 C++ tests cover: identity and fingerprints (including decorated Python
definitions across renames and decorator edits); the Python, JavaScript and
TypeScript parsers; repository ingestion (empty, large, binary, invalid UTF-8,
symlinks, renames, deletions, `.gitignore`, interrupted runs, unreadable files);
the code graph against hand-labelled fixtures (precision and recall both 1.0 on
Python, JavaScript and TypeScript); hybrid retrieval and incremental index
maintenance; automatic evidence invalidation and re-anchoring; memory rules
(revision conflicts, 40-agent concurrency, transitions, approvals, evidence
rollback, restart, isolation, retention, byte-identical export and import); the
HTTP API and static web mount security; backup and restore; observability and
privacy-preserving query logs; the single-writer lock; and CLI integration.

**Measured on public repositories** (shallow clones, cloud sandbox):

| Project | Python files | Symbols | First index | Re-index |
|---|---|---|---|---|
| psf/requests | 37 | 844 | 0.3 s | 0.0 s |
| pallets/flask | 83 | 1,705 | 0.3 s | 0.0 s |
| django/django | 2,933 | 46,906 | 24 s | 0.3 s |

Graph lookups on django take 5–40 ms, including opening the database. The
results are in `docs/research/`.

## Offline file-localisation benchmark (SWE-bench Lite sample)

On a stratified 25-issue sample across 8 SWE-bench Lite repositories (`flask`,
`seaborn`, `xarray`, `astropy`, `requests`, `pylint`, `sphinx`, `pytest`; not a
held-out test — retrieval weights were tuned against this sample), ranking
repository files directly from the first 512 characters of each raw GitHub issue
description (`problem_statement[:512]`, no LLM):

| Method | Acc@1 | Acc@5 | Acc@10 | MRR |
|---|---:|---:|---:|---:|
| Veylqora hybrid (`first512`, before retrieval fixes) | 12.0% | 40.0% | 52.0% | 0.245 |
| Veylqora lexical (`first512`, before retrieval fixes) | 0.0% | 24.0% | 44.0% | 0.122 |
| Whole-file BM25 baseline (`first512`) | 32.0% | 60.0% | 72.0% | 0.456 |
| **Veylqora lexical (`first512`, current)** | **24.0%** | **48.0%** | **72.0%** | **0.368** |
| **Veylqora hybrid (`first512`, current)** | **40.0%** | **52.0%** | **68.0%** | **0.463** |

Stopword filtering, compound identifier tokenization (`snake_case` and
`camelCase` preserved alongside split sub-tokens), IDF and field weighting
(symbol name 8×, FQN 5×, file path 3×, body 1×), test-file de-boosting when the
query does not mention tests, and per-file diversification (`≤ 8` symbols per
file before overflow) raised `hybrid_first512` top-1 accuracy on these 25 issues
from 12.0% (3/25) to 40.0% (10/25) and MRR from 0.245 to 0.463. Compared with
the whole-file BM25 baseline (32.0%, 8/25), hybrid Acc@1 is a tie within noise
on this 25-issue sample (paired difference +8.0 percentage points, 4 wins / 19
ties / 2 losses; 95% confidence interval roughly −12 to +28 percentage points,
or −8 to +28 points depending on the interval method), and BM25 remains higher
at Acc@5 (60.0% vs 52.0%) and Acc@10 (72.0% vs 68.0%).

In a controlled Release-mode indexing benchmark on an idle machine (alternating
old and new binaries across 3 repeats from a fresh database on 6 repositories:
`flask`, `seaborn`, `requests`, `sphinx`, `astropy`, `pylint`), commit `6562e9b`
is about 2–4% slower than commit `3136b00` on the medium and large repositories
(median old/new ratios: `sphinx` 31.17 s vs 32.08 s, `0.97×`; `astropy` 79.87 s
vs 81.26 s, `0.98×`; `pylint` 25.65 s vs 26.05 s, `0.98×`; `seaborn` 7.02 s vs
7.40 s, `0.95×`; `requests` 1.38 s vs 1.36 s, `1.01×`; `flask` 2.42 s vs 2.98 s,
`0.81×`), because emitting both compound identifiers and split sub-tokens
increases the number of unique tokens written per symbol despite prepared-statement
reuse.

## Agent study results

A controlled study on 10 seeded bugs in click (40 runs, one per task and setup), with an AI coding agent. The agent connected to Veylqora and used its tools in 30 runs (49 calls), and every run completed cleanly.

| Setup | Passed |
|---|---|
| A (no Veylqora tools) | 5/10 |
| B (code search) | 4/10 |
| C (search + memory) | 6/10 |
| D (context package) | 3/10 |

The study does not yet show that the tools raise the pass rate. Ten bugs with one run each is too small. Details and how to run it: [study/README.md](study/README.md).

## Tech stack

| Area | Choice | Why |
|---|---|---|
| Core language | C++20 | Performance and predictable memory use for an indexing service |
| Build | CMake ≥ 3.28, Ninja | Standard for C++; FetchContent pins dependencies by tag or SHA-256 |
| Storage | SQLite 3 (WAL, foreign keys, forward-only migrations) | One file, transactional, no server to run |
| Parsing | tree-sitter v0.27.1 + Python, JavaScript and TypeScript grammars (MIT) | Incremental, error-tolerant, fast |
| HTTP | cpp-httplib v0.62.0 (MIT) | Header-only; loopback API |
| JSON | nlohmann/json v3.12.0 (MIT) | Wire format for the CLI and API |
| MCP server | Python `mcp` SDK (`mcp/veylqora_mcp/server.py`) | Exposes code search, graph, memory and context tools over stdio |
| Tests | Catch2 v3.11.0, pytest, Vitest, Playwright | C++, MCP, adapter and browser test suites |
| Quality gates | `-Wall -Wextra -Wconversion … -Werror`, clang-format, ASan + UBSan | Warnings are errors for first-party code |
| Web UI | React 19, TypeScript 5.9, Vite 7, Vitest | Read-only dashboard served by the daemon |
| CI | Cloud sandbox running `scripts/ci.sh` (no hosted runner) | Reproducible; see ADR-0005 |

Planned, not yet in the code: USearch ANN index (ADR-0002) and a learned
embedding model to replace deterministic token-hash vectors for semantic search.

## Current implementation

Each module is certified by `scripts/certify.py`, which runs the checks at a pushed commit and
records the result under `docs/certification/`.

| Module | What it delivers | Status |
|---|---|---|
| M0.1 | Build skeleton, warnings-as-errors, sanitizer wiring, dependency pins | PASS (`ea0c9f8`) |
| M0.2 | Certification system: `certify.py`, `verify_certs.py`, tamper detection | PASS (module 0.1) |
| S1–S4 | Spikes: symbol identity, evidence anchoring, vector delete/recall, MCP handshake | Decided in ADR-0003, 0002, 0007 |
| M1.1 | Core runtime, SQLite schema and migrations, Python indexing, CLI slice | PASS (`33d38a0`) |
| M1.2 | Git-aware ingestion, classification, batched and cancellable indexing | PASS (`f96228c`) |
| M1.3a | Python code graph: tree-sitter symbols, edges with confidence, queries | PASS (`5d89627`) |
| M1.4 | Memory engine: revisions, CAS, state machine, evidence, retention, JSONL export/import | PASS (`2c84c51`) |
| M1.5 | Local HTTP API, `veylqora` CLI with `--json`, single-writer file lock | PASS (`6027d9a`) |
| M1.6 | Read-only React dashboard served by the daemon | PASS (`19f50cf`) |
| M1.3b | JavaScript and TypeScript graph: tree-sitter JS and TS, per-language labelled gate (ADR-0019) | PASS (`25d3a7f`) |
| Phase 2 | Hybrid retrieval, incremental index maintenance, evidence invalidation, MCP server (two named clients), context packaging, multi-agent coordination, observability | PASS: M2.1–M2.7 certified (M2.5 at `dc6628e`, M2.7 at `903a267`) |
| Phase 3 | Hardening, benchmarks, packaging and Docker, offline evaluation | Partly done: M3.1, M3.2, M3.3, M3.4, M3.5a and M3.6a certified |
| M3.5 | PolyFabric and StructZero adapters | M3.5a (generic outbound bridge) certified; product adapters blocked: need their contract documents |
| M3.6b | Agent comparison study | Full study run (40 runs); results in [study/README.md](study/README.md); a larger study is planned |

### Command line

```bash
veylqora init --db veylqora.db
veylqora index ./my-repo --db veylqora.db
veylqora search verify_token --root ./my-repo --db veylqora.db
veylqora graph verify_token --root ./my-repo --db veylqora.db
veylqora status --root ./my-repo --db veylqora.db

veylqora memory add --root ./my-repo --db veylqora.db --type decision \
  --content "tokens are prefixed" --author agent-a --evidence "auth/tokens.py:auth.tokens.verify_token"
veylqora memory list --root ./my-repo --db veylqora.db
veylqora memory inspect <id> --root ./my-repo --db veylqora.db
veylqora memory export memories.jsonl --root ./my-repo --db veylqora.db

veylqora serve --db veylqora.db --port 8765     # daemon; the only writer while running
veylqora status --root ./my-repo --server http://127.0.0.1:8765
```

Add `--json` to any command for machine-readable output. Exit codes: `0` ok,
`1` error, `2` usage, `3` not found, `4` conflict.

### HTTP API (loopback only)

`GET /api/v1/health`, `GET /api/v1/status`, `POST /api/v1/index`,
`GET /api/v1/search`, `GET /api/v1/retrieve`, `GET /api/v1/graph`,
`GET /api/v1/doctor`, `GET /api/v1/metrics`, `GET|POST /api/v1/memories`,
`GET /api/v1/memories/{id}`, `POST /api/v1/memories/{id}/update`,
`POST /api/v1/memories/{id}/transition`, `POST /api/v1/memories/{id}/supersede`,
`POST /api/v1/memories/{id}/accept`, `POST /api/v1/memories/{id}/approve`,
`POST /api/v1/revalidate`, `POST /api/v1/backup`.

Errors are returned as `{"error":{"code":"…","message":"…"}}` with a status of
400 (invalid), 404 (not found) or 409 (revision conflict).

## Known limitations

- **Languages.** Python, JavaScript and TypeScript are indexed (M1.3b). Other languages are not.
- **Name-based, not type-based.** Calls on objects of unknown type are heuristic.
  Dynamic dispatch, `getattr`, and runtime imports are not modelled.
- **Vector search is token-hashed, not a learned embedding model.** Phase 2
  ships hybrid retrieval (IDF-weighted lexical + 256-dim FNV-1a token-hash
  vectors + one-hop graph RRF, ADR-0015), so queries that share no identifiers
  or sub-tokens with a target definition cannot be matched semantically yet.
- **Conflicting memories are not detected automatically.** Re-indexing
  automatically flags memories whose cited symbols changed (`suspected_stale`)
  or disappeared (`evidence_missing`), and re-anchors renamed or moved symbols
  whose body is unchanged (ADR-0014), but semantic contradictions between two
  memories require human or agent review.
- **No secret redaction yet.** Indexed source and memory content are stored as
  given. Redaction at ingest is planned.
- **No authentication.** The HTTP API accepts loopback connections only. Remote
  exposure is refused until authentication exists (ADR-0012 is planned).
- **Storage cost.** The graph roughly triples indexing time and multiplies storage
  by about eight on django. Recorded in `docs/research/graph-m1.3a.txt`.
- **Windows.** Both POSIX (`flock`) and Windows (`LockFileEx`) enforce the
  single-writer database lock, and the CLI builds with MSVC in principle, but CI
  and the sanitizer gates run on Linux only (ADR-0001).

## Decisions and documentation

| File | Contents |
|---|---|
| [`docs/adr/`](docs/adr/) | Architecture decisions, each with evidence and reversal criteria |
| [`docs/certification/`](docs/certification/) | Certification manifests and logs per module and commit |
| [`docs/research/`](docs/research/) | Measurements on public repositories and spike results |
| [`docs/SECURITY.md`](docs/SECURITY.md) | Threat model, controls in place, known gaps |

## Build and test

Requirements: CMake ≥ 3.28, Ninja, a C++20 compiler (gcc 13+ or clang 17+),
SQLite development headers, and network access on the first build (dependencies
are fetched and pinned).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer build:

```bash
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DVEYLQORA_SANITIZER=address+undefined
cmake --build build-san -j && ctest --test-dir build-san --output-on-failure
```

Full gated run, in the cloud sandbox used for CI: `scripts/ci.sh`.

## Roadmap

- **v0.1 (Phase 1 — certified):** Python, JavaScript and TypeScript indexing and code graph; memory engine with revisions, state machine and JSONL export/import; loopback HTTP API, CLI and single-writer lock; read-only React dashboard.
- **v0.2 (Phase 2 — certified):** Hybrid retrieval (IDF-weighted lexical, token-hash vector and one-hop graph RRF); incremental index maintenance; automatic evidence invalidation and re-anchoring; MCP server and token-budgeted context packages; multi-agent coordination and approval workflows; privacy-preserving observability.
- **v0.3 (Phase 3 — in progress):** M3.1–M3.4, M3.5a and M3.6a certified. Remaining work: learned embedding model + USearch ANN index (ADR-0002), ingest-time secret redaction, a larger multi-trial agent study (M3.6b), and product-specific PolyFabric / StructZero adapters once their external contracts are finalized.

## Licence

Apache-2.0. See [`LICENSE`](LICENSE).
