#!/usr/bin/env bash
# Build and test Veylqora in the Docker Cloud Sandbox.
#
# The sandbox is billed per running minute (ADR-0001), so every command here
# is batched and `stop` is always reachable. Usage:
#
#   scripts/sbx.sh up        # start (or create) the sandbox
#   scripts/sbx.sh build     # clone/pull + configure + build + ctest, then report
#   scripts/sbx.sh san       # sanitizer build and test run
#   scripts/sbx.sh shell     # interactive attach
#   scripts/sbx.sh stop      # stop it; run this when you are done
#   scripts/sbx.sh status    # is anything running and costing money?
#
# No secrets are passed in. The sandbox clones the public repo over HTTPS.

set -euo pipefail

SBX_NAME="${SBX_NAME:-shell/veylqora-build}"
REPO_URL="${REPO_URL:-https://github.com/vishalvermauts/Veylqora.git}"
WORKDIR="/home/agent/veylqora"
CPUS="${CPUS:-8}"
MEMORY="${MEMORY:-16g}"
TTL="${TTL:-1h}"

ALLOW_NET='**.debian.org,**.ubuntu.com,github.com,**.github.com,**.githubusercontent.com,codeload.github.com,pypi.org,files.pythonhosted.org,registry.npmjs.org,cmake.org,**.cmake.org,**.kitware.com'

in_sbx() { sbx --cloud exec "$SBX_NAME" -- bash -lc "$1"; }

cmd_up() {
  if sbx --cloud ls | grep -q "${SBX_NAME#shell/}"; then
    sbx --cloud attach "$SBX_NAME" --detach 2>/dev/null || true
  else
    sbx --cloud create shell --name "${SBX_NAME#shell/}" \
      --cpus "$CPUS" --memory "$MEMORY" --platform linux/amd64 \
      --ttl "$TTL" --on-timeout stop --allow-network "$ALLOW_NET"
  fi
  in_sbx 'command -v cmake >/dev/null || scripts/provision.sh' 2>/dev/null || true
}

cmd_provision() {
  in_sbx '
    set -e
    sudo -n mv /etc/apt/sources.list.d/docker.* /tmp/ 2>/dev/null || true
    sudo -n apt-get update -qq
    DEBIAN_FRONTEND=noninteractive sudo -n apt-get install -y -qq --no-install-recommends \
      build-essential cmake ninja-build pkg-config ccache gdb \
      clang clang-tidy clang-format lld llvm \
      libsqlite3-dev sqlite3 curl zip unzip tar python3-venv python3-pip
    gcc --version | head -1; clang --version | head -1; cmake --version | head -1'
}

cmd_sync() {
  in_sbx "
    set -e
    if [ -d $WORKDIR/.git ]; then git -C $WORKDIR fetch --quiet origin && git -C $WORKDIR reset --hard --quiet origin/main
    else git clone --quiet $REPO_URL $WORKDIR; fi
    git -C $WORKDIR log --oneline -1"
}

cmd_build() {
  cmd_sync
  in_sbx "
    set -e
    cd $WORKDIR
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    cmake --build build -j $CPUS
    ctest --test-dir build --output-on-failure"
}

cmd_san() {
  cmd_sync
  in_sbx "
    set -e
    cd $WORKDIR
    cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++ -DVEYLQORA_SANITIZER=address+undefined
    cmake --build build-san -j $CPUS
    ctest --test-dir build-san --output-on-failure"
}

cmd_stop()   { sbx --cloud stop "$SBX_NAME"; sbx --cloud ls; }
cmd_status() { sbx --cloud ls; sbx --cloud ttl "$SBX_NAME" 2>/dev/null || true; }
cmd_shell()  { sbx --cloud attach "$SBX_NAME"; }

case "${1:-}" in
  up)        cmd_up ;;
  provision) cmd_provision ;;
  sync)      cmd_sync ;;
  build)     cmd_build ;;
  san)       cmd_san ;;
  stop)      cmd_stop ;;
  status)    cmd_status ;;
  shell)     cmd_shell ;;
  *) sed -n '2,20p' "$0"; exit 2 ;;
esac
