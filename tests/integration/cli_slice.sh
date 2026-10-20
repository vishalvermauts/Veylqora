#!/usr/bin/env bash
# CLI integration test for M1.1-M1.5. Each `veylqora` call is a separate process,
# so a second run after a process exit is a real restart. Covers the human and
# JSON outputs, exit codes, memories, the daemon, and the single-writer lock.
set -euo pipefail

bin="$1"
fixtures="$2"
tmp=$(mktemp -d)
server_pid=""
cleanup() {
  [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
  rm -rf "$tmp"
}
trap cleanup EXIT
db="$tmp/veylqora.db"

fail() { echo "cli test: $*" >&2; exit 1; }
expect_exit() {  # expect_exit <code> <command...>
  local want="$1"; shift
  set +e; "$@" >/dev/null 2>&1; local got=$?; set -e
  [ "$got" -eq "$want" ] || fail "expected exit $want, got $got: $*"
}

# --- indexing and search, persisted across processes -------------------------
"$bin" index "$fixtures" --db "$db" | grep -q "^indexed 10,"
first=$("$bin" search verify_token --root "$fixtures" --db "$db")
second=$("$bin" search verify_token --root "$fixtures" --db "$db")
[ "$first" = "$second" ] || fail "answers differ after restart"
echo "$first" | grep -q "^auth/tokens.py:8  function  auth.tokens.verify_token$" \
  || fail "unexpected search output: $first"

# --- JSON output is machine-readable ----------------------------------------
json=$("$bin" search verify_token --root "$fixtures" --db "$db" --json)
echo "$json" | grep -q '"fqn": "auth.tokens.verify_token"' || fail "--json search output"

# --- exit codes -------------------------------------------------------------
expect_exit 2 "$bin" bogus --db "$db"
expect_exit 2 "$bin" search
expect_exit 3 "$bin" memory inspect does-not-exist --root "$fixtures" --db "$db"

# --- memories through the CLI -----------------------------------------------
add=$("$bin" memory add --root "$fixtures" --db "$db" --type decision \
  --content "tokens are prefixed" --author agent-a --evidence "auth/tokens.py:auth.tokens.verify_token")
id=$(echo "$add" | cut -d' ' -f1)
echo "$add" | grep -q "  active  revision 1$" || fail "memory add: $add"
"$bin" memory list --root "$fixtures" --db "$db" | grep -q "tokens are prefixed" || fail "memory list"
"$bin" memory inspect "$id" --root "$fixtures" --db "$db" | grep -q "evidence: auth/tokens.py:auth.tokens.verify_token" \
  || fail "memory inspect shows evidence"
expect_exit 4 "$bin" memory update "$id" --root "$fixtures" --db "$db" --expected 7 \
  --content "stale" --actor agent-b --reason "late"
"$bin" memory update "$id" --root "$fixtures" --db "$db" --expected 1 \
  --content "tokens are prefixed v2" --actor agent-b --reason "clarified" | grep -q "revision 2" \
  || fail "memory update"
expect_exit 1 "$bin" memory add --root "$fixtures" --db "$db" --type decision --content x \
  --author a --evidence "auth/tokens.py:no.such.symbol"

# --- export, import into a fresh database, export again ---------------------
"$bin" memory export "$tmp/m1.jsonl" --root "$fixtures" --db "$db" >/dev/null
"$bin" memory import "$tmp/m1.jsonl" --db "$tmp/fresh.db" >/dev/null || true
"$bin" init --db "$tmp/fresh.db" >/dev/null
"$bin" memory import "$tmp/m1.jsonl" --db "$tmp/fresh.db" | grep -q "imported 1 memories" || fail "import"
"$bin" memory export "$tmp/m2.jsonl" --root "$fixtures" --db "$tmp/fresh.db" >/dev/null
cmp -s "$tmp/m1.jsonl" "$tmp/m2.jsonl" || fail "export is not byte-identical after import"

# --- the daemon: single writer, reachable over HTTP ---------------------------
serve_out="$tmp/serve.out"
"$bin" serve --db "$db" --port 0 >"$serve_out" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do grep -q "^listening on" "$serve_out" 2>/dev/null && break; sleep 0.05; done
port=$(sed -n 's#^listening on http://127.0.0.1:\([0-9]*\).*#\1#p' "$serve_out")
[ -n "$port" ] || fail "server did not start"
server_url="http://127.0.0.1:$port"

# While the daemon holds the lock, direct access to the file is refused.
expect_exit 1 "$bin" status --root "$fixtures" --db "$db"
# The same question through the server answers.
"$bin" status --root "$fixtures" --server "$server_url" | grep -q "indexed: yes" || fail "status via server"
"$bin" memory list --root "$fixtures" --server "$server_url" | grep -q "tokens are prefixed v2" \
  || fail "memory list via server"
expect_exit 3 "$bin" memory inspect nope --root "$fixtures" --server "$server_url"

kill "$server_pid"; wait "$server_pid" 2>/dev/null || true; server_pid=""

# --- version -----------------------------------------------------------------
"$bin" --version | grep -q "^veylqora " || fail "--version"
echo "cli integration: ok"
