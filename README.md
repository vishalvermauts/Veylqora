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
| gcc 15 build, `-Werror`, all tests | PASS — 105 / 105 (measured 2026-10-22 at `6562e9b`) |
| clang 21 build, `-Werror`, all tests | PASS — 105 / 105 (measured 2026-10-22 at `6562e9b`) |
| AddressSanitizer + UndefinedBehaviorSanitizer, all tests | PASS — 105 / 105 (measured 2026-10-22 at `6562e9b`) |
| MCP server end-to-end (`pytest mcp/tests`) | PASS — 20 / 20 (measured 2026-10-22 at `6562e9b`) |
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

_The three rows above were measured earlier (2026-10-20, older build, not on an idle machine). Re-measure before relying on the timings._

Graph lookups on django take 5–40 ms, including opening the database. The
results are in `docs/research/`.

## File-localisation benchmark on real bug reports (SWE-bench Lite)

**What is measured:** given the text of a real GitHub bug report, rank the repository's Python files and check where the file changed by the real fix lands. No language model is used. Each issue is checked out at its own base commit.

**Held-out result (113 issues, not used for tuning):** commit `0239d13` (binary SHA-256 `e6dbedae…8506`, the same binary at `f488f3f`), GCC 15.2, Release build. Queries are the first 512 characters of each issue.

| Method | Acc@1 | Acc@5 | Acc@10 | MRR |
|---|---:|---:|---:|---:|
| Veylqora lexical (first 512) | 40.7% | 66.4% | 77.0% | 0.529 |
| Veylqora hybrid (first 512) | 39.8% | 67.3% | 75.2% | 0.520 |
| BM25 over files (first 512) | 23.0% | 53.1% | 63.7% | 0.380 |
| Veylqora hybrid, before these changes (`6562e9b`) | 20.4% | 44.2% | 56.6% | 0.325 |

Paired difference, hybrid minus BM25 (first 512), with 95% bootstrap intervals over issues: Acc@1 +16.8 points [+7.1, +25.7]; Acc@5 +14.2 [+4.4, +23.9]; Acc@10 +11.5 [+2.7, +20.4]; MRR +0.140 [+0.068, +0.213]. Every interval excludes zero.

**Limits:** the 113 issues were not used for tuning, but they come from the same benchmark as the earlier 138-issue diagnostic sample, so the set is not fully blind. The tuning set (25 issues) was used to choose the changes. Two repositories are only 1 to 3 issues. An independent rerun by someone other than the author is still pending.

**Speed:** indexing time, median of 5 alternating runs, Release build, quiet machine, same checkouts. Old is `6562e9b`, new is `0239d13`:

| Repository (files) | Before | After | Change |
|---|---:|---:|---:|
| pylint (1,093) | 25.75 s | 2.17 s | 11.9× faster |
| sphinx (658) | 31.64 s | 3.03 s | 10.4× faster |
| astropy (914) | 83.89 s | 10.59 s | 7.9× faster |

**Same answers:** across 6 checkouts and 24 queries, the indexed files, symbol counts, token rows and all 48 ranked lists are identical before and after the speed change, with one compiler (GCC 15.2).

**Certification:** CI jobs (format, gcc, clang, sanitizer, mcp, eval, e2e, web, adapters) pass at `0239d13`. Certification records for modules 1.1, 2.1, 2.2 and 3.6a are in `docs/certification/` and verify with `scripts/verify_certs.py`.

*These results are on the code in PR #6 (`feat/search-and-indexing-speed`). They describe `main` only after that PR is merged.*

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
