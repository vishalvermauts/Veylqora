#!/usr/bin/env bash
# Veylqora CI — runs in a dedicated Docker Cloud Sandbox (ADR-0005).
#
# No hosted runner and no local engine: CI is a second cloud sandbox,
# `shell/veylqora-ci`, kept separate from the development sandbox so that a CI
# run always starts from a clean clone of origin/main rather than from whatever
# a developer left in their tree.
#
# The sandbox is BILLED PER RUNNING MINUTE. Therefore:
#   * every run is batched into one non-interactive `sbx exec`;
#   * the sandbox is STOPPED at the end of every run, including on failure;
#   * `--keep` is the only way to leave it running, and it says so loudly.
#
#   scripts/ci.sh                   # all gating jobs on origin/main, then stop
#   scripts/ci.sh --ref HEAD        # test the pushed commit you are on
#   scripts/ci.sh format gcc        # named jobs only
#   scripts/ci.sh --clean           # wipe the clone and dependency cache first
#   scripts/ci.sh --keep gcc        # leave the sandbox running afterwards
#   scripts/ci.sh --status          # is anything costing money right now?
#   scripts/ci.sh --native [jobs]   # run the jobs here (used inside the sandbox)
#
# Exit code is the number of failed jobs.

set -uo pipefail

SBX="${VEYLQORA_CI_SBX:-shell/veylqora-ci}"
REPO_URL="${VEYLQORA_REPO:-https://github.com/vishalvermauts/Veylqora.git}"
CHECKOUT=/home/agent/ci
CPUS="${CPUS:-4}"
MEMORY="${MEMORY:-8g}"
TTL="${TTL:-2h}"  # long enough for a one-hour fuzz run (see run_remote)
ALL_JOBS=(format gcc clang san certs mcp fuzz eval e2e web)

# Hosts the jobs reach. The sandbox's own proxy enforces this list when it is created.
ALLOW_NET='**.debian.org,**.ubuntu.com,github.com,**.github.com,**.githubusercontent.com,codeload.github.com,pypi.org,files.pythonhosted.org,registry.npmjs.org,**.npmjs.org'

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ok()  { printf '\033[32m   PASS\033[0m  %s\n' "$*"; }
bad() { printf '\033[31m   FAIL\033[0m  %s\n' "$*"; }

# ============================ jobs (run inside the sandbox) ==================

SRC="${SRC:-$CHECKOUT}"
WORK="${WORK:-/home/agent/ci-work}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

job_format() {
  local files
  files=$(find "$SRC/include" "$SRC/src" "$SRC/tests" \
            \( -name '*.hpp' -o -name '*.cpp' \) 2>/dev/null)
  [ -z "$files" ] && { echo "no sources to check"; return 0; }
  # shellcheck disable=SC2086
  clang-format --dry-run --Werror $files
}

_build_and_test() {          # _build_and_test <name> <cmake args...>
  local dir="$WORK/$1"; shift
  mkdir -p "$WORK"
  cmake -S "$SRC" -B "$dir" -G Ninja \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    "$@" >"$dir.configure.log" 2>&1 || { tail -30 "$dir.configure.log"; return 1; }
  cmake --build "$dir" -j "$JOBS" >"$dir.build.log" 2>&1 \
    || { tail -40 "$dir.build.log"; return 1; }
  ctest --test-dir "$dir" --output-on-failure
}

job_gcc()   { CC=gcc   CXX=g++     _build_and_test build-gcc   -DCMAKE_BUILD_TYPE=RelWithDebInfo; }
job_clang() { CC=clang CXX=clang++ _build_and_test build-clang -DCMAKE_BUILD_TYPE=RelWithDebInfo; }

# MSVC has no UBSan, which is why the sanitizer gate lives on this platform
# only (ADR-0001).
job_san() {
  _build_and_test build-san -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER=clang++ -DVEYLQORA_SANITIZER=address+undefined
}

# Verifies committed certification evidence. Non-strict: a module that has not
# been re-certified at HEAD is a warning here; release gating uses --strict.
job_certs() { python3 "$SRC/scripts/verify_certs.py"; }

# The MCP server, tested end to end: the real daemon, the MCP server as a process,
# and the MCP SDK client over stdio (M2.5). Needs package-index access.
job_mcp() {
  local venv="$WORK/mcp-venv"
  mkdir -p "$WORK"
  # A venv left half-built by an earlier failed run is rebuilt, not reused.
  if [ ! -x "$venv/bin/pip" ]; then
    rm -rf "$venv"
    python3 -m venv "$venv" || return 1
  fi
  "$venv/bin/python" -m pip install -q -r "$SRC/mcp/requirements.txt" || return 1
  job_gcc >/dev/null || return 1
  VEYLQORA_BIN="$WORK/build-gcc/src/veylqora" VEYLQORA_FIXTURES="$SRC/tests/fixtures/python" \
    "$venv/bin/python" -m pytest -q "$SRC/mcp/tests"
}

# Offline evaluation (M3.6a): retrieval and stale-detection gates against the
# committed datasets. The result is printed so it is kept in the job log.
job_eval() {
  job_gcc >/dev/null || return 1
  VEYLQORA_BIN="$WORK/build-gcc/src/veylqora" python3 "$SRC/scripts/evaluate.py"     --out "$WORK/results.json" || { cat "$WORK/results.json" 2>/dev/null; return 1; }
  cat "$WORK/results.json"
}

# End-to-end packaging check (M3.4): install, start, index, CLI search, MCP
# connect, restart, backup and restore, in a clean directory. Uses the MCP venv.
job_e2e() {
  local venv="$WORK/mcp-venv"
  mkdir -p "$WORK"
  if [ ! -x "$venv/bin/pip" ]; then
    rm -rf "$venv"
    python3 -m venv "$venv" || return 1
  fi
  "$venv/bin/python" -m pip install -q -r "$SRC/mcp/requirements.txt" || return 1
  job_gcc >/dev/null || return 1
  bash "$SRC/scripts/e2e.sh" "$WORK/build-gcc/src/veylqora" "$SRC/tests/fixtures/python"     "$venv/bin/python"
}

# Performance benchmarks (M3.2). A Release build, because a debug or instrumented
# build measures the wrong thing. Not in the default set: it takes minutes.
job_bench() {
  local dir="$WORK/build-bench"
  mkdir -p "$WORK"
  cmake -S "$SRC" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DVEYLQORA_BUILD_TESTS=OFF     >"$dir.configure.log" 2>&1 || { tail -30 "$dir.configure.log"; return 1; }
  cmake --build "$dir" --target veylqora -j "$JOBS" >"$dir.build.log" 2>&1     || { tail -40 "$dir.build.log"; return 1; }
  VEYLQORA_BIN="$dir/src/veylqora" python3 "$SRC/scripts/bench.py" --out "$WORK/bench-out"     || return 1
  echo "BENCH-RESULTS-BEGIN"
  cat "$WORK/bench-out/results.json"
  echo "BENCH-RESULTS-END"
}

# Web dashboard tests and production build (M3.1). Node comes from a pinned
# PyPI wheel, because the Ubuntu package is older than the Vite toolchain needs.
job_web() {
  local venv="$WORK/node-venv"
  mkdir -p "$WORK"
  if [ ! -x "$venv/bin/pip" ]; then
    rm -rf "$venv"
    python3 -m venv "$venv" || return 1
  fi
  "$venv/bin/python" -m pip install -q "nodejs-wheel-binaries==22.20.0" || return 1
  local node_dir
  node_dir="$("$venv/bin/python" -c 'import nodejs_wheel, os; print(os.path.join(os.path.dirname(nodejs_wheel.__file__), "bin"))' 2>/dev/null)"
  export PATH="$venv/bin:${node_dir}:$PATH"
  node --version >/dev/null || return 1
  # The wheel's own `npm` launcher is broken; run npm's entry point through its node.
  local npm_cli="${node_dir%/bin}/lib/node_modules/npm/bin/npm-cli.js"
  [ -f "$npm_cli" ] || { echo "error: npm not found in the Node wheel" >&2; return 1; }
  cd "$SRC/web" || return 1
  node "$npm_cli" ci --no-audit --no-fund >"$WORK/npm-ci.log" 2>&1     || { tail -30 "$WORK/npm-ci.log"; return 1; }
  node "$npm_cli" run build || return 1
  node "$npm_cli" test
}

# Container image (M3.4): build it, start it, wait for its health check, then run
# the CLI and the MCP check inside the container against a read-only fixture.
job_container() {
  local image="veylqora:ci" name="veylqora-ci-test" rc=0
  docker build -t "$image" "$SRC" >"$WORK/docker-build.log" 2>&1     || { tail -40 "$WORK/docker-build.log"; return 1; }
  docker rm -f "$name" >/dev/null 2>&1 || true
  docker run -d --name "$name" -v "$SRC/tests/fixtures/python:/workspace:ro" "$image" >/dev/null     || return 1
  local health="" i
  for i in $(seq 1 60); do
    health=$(docker inspect -f '{{.State.Health.Status}}' "$name" 2>/dev/null)
    [ "$health" = healthy ] && break
    sleep 2
  done
  if [ "$health" != healthy ]; then
    echo "container did not become healthy (status: ${health:-unknown})"
    docker logs "$name" 2>&1 | tail -40
    docker rm -f "$name" >/dev/null 2>&1
    return 1
  fi
  docker exec "$name" veylqora index /workspace --server http://127.0.0.1:8765 || rc=1
  docker exec "$name" veylqora search verify_token --root /workspace --server http://127.0.0.1:8765     | grep -q "auth.tokens.verify_token" || { echo "search in the container missed verify_token"; rc=1; }
  docker exec -e VEYLQORA_FIXTURES=/workspace "$name" python3 /opt/veylqora/scripts/e2e_mcp.py     || { echo "MCP check in the container failed"; rc=1; }
  docker rm -f "$name" >/dev/null 2>&1
  return "$rc"
}

job_tidy() {
  [ -f "$WORK/build-clang/compile_commands.json" ] || job_clang >/dev/null || return 1
  find "$SRC/src" -name '*.cpp' -print0 \
    | xargs -0 -r clang-tidy -p "$WORK/build-clang" --quiet
}

# Fuzz smoke run (M3.3): each target runs for FUZZ_SECONDS from a corpus seeded
# with the fixtures, under ASan and UBSan. A crash fails the job, and the input
# that caused it is kept in $WORK/fuzz-artifacts for inspection.
FUZZ_SECONDS="${FUZZ_SECONDS:-60}"
FUZZ_TARGETS=(fuzz_python_parser fuzz_javascript_parser fuzz_retrieval_query fuzz_memory_json)

job_fuzz() {
  local dir="$WORK/build-fuzz" failed=0 t corpus artifacts
  CC=clang CXX=clang++ cmake -S "$SRC" -B "$dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVEYLQORA_FUZZ=ON -DVEYLQORA_BUILD_TESTS=OFF \
    -DVEYLQORA_SANITIZER=address+undefined \
    >"$dir.configure.log" 2>&1 || { tail -30 "$dir.configure.log"; return 1; }
  cmake --build "$dir" -j "$JOBS" --target "${FUZZ_TARGETS[@]}" \
    >"$dir.build.log" 2>&1 || { tail -40 "$dir.build.log"; return 1; }
  for t in "${FUZZ_TARGETS[@]}"; do
    corpus="$WORK/corpus-$t"
    artifacts="$WORK/fuzz-artifacts/$t"
    rm -rf "$corpus" "$artifacts"
    mkdir -p "$corpus" "$artifacts"
    cp -r "$SRC/tests/fixtures/." "$corpus/"
    say "fuzz $t ($FUZZ_SECONDS s)"
    if "$dir/tests/fuzz/$t" "$corpus" -max_total_time="$FUZZ_SECONDS" \
         -artifact_prefix="$artifacts/" -print_final_stats=1 >"$WORK/$t.fuzz.log" 2>&1; then
      tail -2 "$WORK/$t.fuzz.log"
    else
      tail -40 "$WORK/$t.fuzz.log"
      failed=$((failed + 1))
    fi
  done
  return "$failed"
}

run_native() {
  local failed=0
  for job in "$@"; do
    say "$job"
    if "job_$job"; then ok "$job"; else bad "$job"; failed=$((failed + 1)); fi
  done
  say "summary"
  if [ "$failed" -eq 0 ]; then ok "all jobs passed"; else bad "$failed job(s) failed"; fi
  return "$failed"
}

# ========================= sandbox orchestration (host side) ================

sbx_exists() { sbx --cloud ls 2>/dev/null | grep -q "[[:space:]]${SBX#shell/}[[:space:]]\|/${SBX#shell/}[[:space:]]"; }

sbx_ensure() {
  if ! sbx_exists; then
    say "creating $SBX ($CPUS vCPU / $MEMORY)"
    sbx --cloud create shell --name "${SBX#shell/}" \
      --cpus "$CPUS" --memory "$MEMORY" --platform linux/amd64 \
      --ttl "$TTL" --on-timeout stop --allow-network "$ALLOW_NET" -q || return 1
  fi
}

# Provision once; the filesystem survives `sbx stop`, so later runs skip this.
sbx_provision() {
  sbx --cloud exec "$SBX" -- bash -lc '
    set -e
    sudo -n mv /etc/apt/sources.list.d/docker.* /tmp/ 2>/dev/null || true
    # A new sandbox can hold the apt lock while its first-boot updates run: retry.
    apt_retry() { for n in 1 2 3 4 5 6 7 8 9 10 11 12; do "$@" && return 0; sleep 15; done; return 1; }
    apt_retry sudo -n apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt_retry sudo -n apt-get install -y -qq --no-install-recommends \
      build-essential cmake ninja-build pkg-config ccache \
      clang clang-tidy clang-format lld llvm libsqlite3-dev sqlite3 \
      libclang-rt-dev python3-venv python3-pip
    gcc --version | head -1; clang --version | head -1; cmake --version | head -1'
}

# A sandbox that is still stopping refuses exec ("is stopping"). Wait until it
# has settled into running or stopped before using it.
sbx_settle() {
  # The listing can show "stopped" while a shutdown is still finishing, and exec
  # is refused until it does. So probe with a real exec until one is accepted.
  sbx_exists || return 0  # absent: sbx_ensure creates it
  local i
  for i in $(seq 1 60); do
    if sbx --cloud exec "$SBX" -- true >/dev/null 2>&1; then
      sleep 5  # one more pause: an accepted probe can still be followed by a shutdown
      sbx --cloud exec "$SBX" -- true >/dev/null 2>&1 && return 0
    fi
    sleep 15
  done
  echo "error: $SBX would not accept commands (is it stopping?)" >&2
  return 1
}

sbx_stop() { say "stopping $SBX"; sbx --cloud stop "$SBX" >/dev/null 2>&1 || true; }

run_remote() {
  local ref="$1" clean="$2" keep="$3"; shift 3
  sbx_settle   || return 1
  sbx_ensure   || return 1
  sbx_provision || return 1
  # The sandbox stops when its TTL runs out, even in the middle of a run. A fuzz
  # run is longer than the default 30 minutes, so extend the TTL first (the
  # server ceiling is 24 hours from creation). Failure here is not fatal.
  if [[ " $* " == *" fuzz "* ]]; then
    if ! sbx --cloud ttl "+$(( FUZZ_SECONDS * ${#FUZZ_TARGETS[@]} / 60 + 60 ))m" "$SBX" >/dev/null 2>&1; then
      echo "error: could not extend the TTL of $SBX for the fuzz run; not starting it" >&2
      return 1
    fi
  fi
  [ "$keep" = 1 ] || trap sbx_stop EXIT

  sbx --cloud exec "$SBX" -- bash -lc "
    set -e
    $([ "$clean" = 1 ] && echo "rm -rf $CHECKOUT /home/agent/ci-work")
    if [ -d $CHECKOUT/.git ]; then
      git -C $CHECKOUT fetch --quiet --prune origin
    else
      git clone --quiet $REPO_URL $CHECKOUT
    fi
    git -C $CHECKOUT reset --hard --quiet $ref
    git -C $CHECKOUT clean -xfdq
    echo \"testing \$(git -C $CHECKOUT log --oneline -1)\"
    FUZZ_SECONDS=$FUZZ_SECONDS bash $CHECKOUT/scripts/ci.sh --native $*"
}

# ================================== dispatch ================================

native=0 clean=0 keep=0 ref="origin/main"
while [ $# -gt 0 ]; do
  case "$1" in
    --native) native=1; shift ;;
    --clean)  clean=1;  shift ;;
    --keep)   keep=1;   shift ;;
    --ref)    ref="$2"; shift 2 ;;
    --status) sbx --cloud ls; exit 0 ;;
    --stop)   sbx_stop; exit 0 ;;
    --list)   printf '%s\n' "${ALL_JOBS[@]}" tidy; exit 0 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) break ;;
  esac
done

jobs=("$@")
[ "${#jobs[@]}" -eq 0 ] && jobs=("${ALL_JOBS[@]}")
for j in "${jobs[@]}"; do
  declare -F "job_$j" >/dev/null || { echo "unknown job: $j (try --list)" >&2; exit 2; }
done

if [ "$native" -eq 1 ]; then
  run_native "${jobs[@]}"
else
  run_remote "$ref" "$clean" "$keep" "${jobs[@]}"
  rc=$?
  [ "$keep" = 1 ] && printf '\n\033[33m!! %s is still RUNNING and billing. scripts/ci.sh --stop\033[0m\n' "$SBX"
  exit $rc
fi
