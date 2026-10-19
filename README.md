# Veylqora

Code intelligence and persistent agent memory: a single-node C++20 service that
indexes a repository's structure, stores an agent's durable knowledge about it,
and — the part that is actually novel — tracks which stored memories a code
change puts in doubt, rather than guessing or ignoring the question.

**Status: pre-v0.1.** The toolchain, dependency strategy, identity model and
certification regime are decided and building. Nothing else is implemented yet.
Do not read the plan as a description of working software.

## Documents

| File | What it is |
|---|---|
| [`PLAN_REVIEW.md`](PLAN_REVIEW.md) | Critique of the original directive: flaws, ranked by cost of not fixing |
| [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) | The plan being executed: phases, data model, logic, gates |
| [`docs/adr/`](docs/adr/) | Decisions, with the evidence and the reversal criteria |

## Building

Linux is the reference platform (ADR-0001). Requires CMake ≥ 3.28, a C++20
compiler and SQLite development headers.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer build (Clang on Linux only — MSVC has no UBSan):

```bash
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DVEYLQORA_SANITIZER=address+undefined
cmake --build build-san -j && ctest --test-dir build-san --output-on-failure
```

Builds run in a Docker Cloud Sandbox, which is **billed per running minute**:

```bash
scripts/sbx.sh up && scripts/sbx.sh build && scripts/sbx.sh stop
```

`scripts/sbx.sh stop` is not optional. `scripts/sbx.sh status` shows whether
anything is still running.

## Licence

Apache-2.0. See [`LICENSE`](LICENSE).
