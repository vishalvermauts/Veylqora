#!/usr/bin/env bash
# CLI smoke test for the M1.1 slice. Each `veylqora` call is a separate process,
# so the second search runs after a real restart.
set -euo pipefail

bin="$1"
fixtures="$2"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
db="$tmp/veylqora.db"

"$bin" index "$fixtures" --db "$db" | grep -q "indexed 10 file(s)"

first=$("$bin" search verify_token --root "$fixtures" --db "$db")
second=$("$bin" search verify_token --root "$fixtures" --db "$db")
[ "$first" = "$second" ] || { echo "answers differ after restart" >&2; exit 1; }
echo "$first" | grep -q "^auth/tokens.py:8  function  auth.tokens.verify_token$" \
  || { echo "unexpected search output: $first" >&2; exit 1; }

set +e
"$bin" bogus --db "$db" 2>/dev/null
rc=$?
set -e
[ "$rc" -eq 2 ] || { echo "usage error exit code was $rc, expected 2" >&2; exit 1; }

"$bin" --version | grep -q "^veylqora "
echo "cli slice: ok"
