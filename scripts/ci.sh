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
TTL="${TTL:-30m}"
ALL_JOBS=(format gcc clang san)

ALLOW_NET='**.debian.org,**.ubuntu.com,github.com,**.github.com,**.githubusercontent.com,codeload.github.com'

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

job_tidy() {
  [ -f "$WORK/build-clang/compile_commands.json" ] || job_clang >/dev/null || return 1
  find "$SRC/src" -name '*.cpp' -print0 \
    | xargs -0 -r clang-tidy -p "$WORK/build-clang" --quiet
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
    command -v cmake >/dev/null && command -v clang-format >/dev/null && exit 0
    set -e
    sudo -n mv /etc/apt/sources.list.d/docker.* /tmp/ 2>/dev/null || true
    sudo -n apt-get update -qq
    DEBIAN_FRONTEND=noninteractive sudo -n apt-get install -y -qq --no-install-recommends \
      build-essential cmake ninja-build pkg-config ccache \
      clang clang-tidy clang-format lld llvm libsqlite3-dev sqlite3
    gcc --version | head -1; clang --version | head -1; cmake --version | head -1'
}

sbx_stop() { say "stopping $SBX"; sbx --cloud stop "$SBX" >/dev/null 2>&1 || true; }

run_remote() {
  local ref="$1" clean="$2" keep="$3"; shift 3
  sbx_ensure   || return 1
  sbx_provision || return 1
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
    bash $CHECKOUT/scripts/ci.sh --native $*"
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
