#!/usr/bin/env bash
# End-to-end packaging check (M3.4): a clean directory, then install -> start ->
# index -> CLI search -> MCP connect -> restart -> backup -> restore -> search again.
# Usage: scripts/e2e.sh <veylqora binary> <fixture root> <python with mcp installed>
# Exits non-zero at the first step that does not match what it should.

set -euo pipefail

BIN="$1"
FIXTURES="$2"
PY="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
PORT=18765
URL="http://127.0.0.1:${PORT}"
DAEMON_PID=""

step() { printf '\n== %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

stop_daemon() {
  if [ -n "$DAEMON_PID" ]; then
    kill "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
    DAEMON_PID=""
  fi
}
trap 'stop_daemon; rm -rf "$WORK"' EXIT

start_daemon() {  # start_daemon <db>
  "$BIN" serve --db "$1" --port "$PORT" >"$WORK/daemon.log" 2>&1 &
  DAEMON_PID=$!
  for _ in $(seq 1 100); do
    if "$PY" -c "import urllib.request,sys; urllib.request.urlopen('${URL}/api/v1/health', timeout=1)" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  fail "daemon did not answer on ${URL}"
}

step "install: the binary initialises a clean database"
"$BIN" init --db "$WORK/live.db" >/dev/null
[ -f "$WORK/live.db" ] || fail "init did not create the database"

step "start: the daemon answers its health check"
start_daemon "$WORK/live.db"

step "index: the fixture through the daemon"
"$BIN" index "$FIXTURES" --server "$URL" >"$WORK/index.out"
grep -q "indexed" "$WORK/index.out" || fail "index did not report indexed files"

step "CLI search: a known definition is found"
"$BIN" search verify_token --root "$FIXTURES" --server "$URL" >"$WORK/search1.out"
grep -q "auth.tokens.verify_token" "$WORK/search1.out" || fail "CLI search did not find verify_token"

step "MCP connect: a real MCP client lists the tools and calls one"
VEYLQORA_URL="$URL" VEYLQORA_FIXTURES="$FIXTURES" "$PY" "$HERE/e2e_mcp.py" >"$WORK/mcp.out" \
  || fail "MCP client check failed"
cat "$WORK/mcp.out"

step "restart: the daemon comes back on the same database with the same answer"
stop_daemon
start_daemon "$WORK/live.db"
"$BIN" search verify_token --root "$FIXTURES" --server "$URL" >"$WORK/search2.out"
diff -u "$WORK/search1.out" "$WORK/search2.out" || fail "search changed across a restart"

step "backup: a checked snapshot of the stopped database"
stop_daemon
"$BIN" backup "$WORK/snapshot.db" --db "$WORK/live.db" >/dev/null

step "restore: the snapshot is restored into a new database and served"
"$BIN" restore "$WORK/snapshot.db" --db "$WORK/restored.db" >/dev/null
start_daemon "$WORK/restored.db"
"$BIN" search verify_token --root "$FIXTURES" --server "$URL" >"$WORK/search3.out"
diff -u "$WORK/search1.out" "$WORK/search3.out" || fail "search changed after a restore"

step "all steps passed"
