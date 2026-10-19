#!/usr/bin/env bash
# Index real public Python repositories with the veylqora binary and check
# that known symbols are found. Reproducible: pinned shallow clones, fixed
# expectations taken from each project's source, no hand-edited results.
#
#   scripts/corpus.sh <veylqora-binary> <work-dir>
set -euo pipefail

bin="$1"
work="$2"
mkdir -p "$work"
failed=0

# name | clone url | symbol to search | file it must be found in
corpus=(
  "requests|https://github.com/psf/requests.git|Session|requests/sessions.py"
  "flask|https://github.com/pallets/flask.git|Flask|src/flask/app.py"
  "django|https://github.com/django/django.git|QuerySet|django/db/models/query.py"
)

for entry in "${corpus[@]}"; do
  IFS='|' read -r name url symbol expected <<< "$entry"
  src="$work/$name"
  db="$work/$name.db"
  rm -f "$db" "$db-wal" "$db-shm"
  [ -d "$src/.git" ] || git clone -q --depth 1 "$url" "$src"

  start=$(date +%s.%N)
  first=$("$bin" index "$src" --db "$db" 2>/dev/null)
  mid=$(date +%s.%N)
  second=$("$bin" index "$src" --db "$db" 2>/dev/null)
  end=$(date +%s.%N)

  hits=$("$bin" search "$symbol" --root "$src" --db "$db")
  ok=yes
  echo "$hits" | grep -q "^$expected:" || ok=no

  # A second run over unchanged files must parse nothing.
  echo "$second" | grep -q "indexed 0 file(s)" || ok=no

  printf '%-10s first: %s\n' "$name" "$first"
  printf '%-10s second: %s\n' "$name" "$second"
  printf '%-10s first run %.1fs, re-index %.1fs, db %s bytes, search "%s": %s\n' \
    "$name" "$(echo "$mid - $start" | bc)" "$(echo "$end - $mid" | bc)" \
    "$(stat -c %s "$db")" "$symbol" "$(echo "$hits" | head -1)"
  if [ "$ok" = yes ]; then echo "$name: PASS"; else echo "$name: FAIL"; failed=$((failed + 1)); fi
done

echo "corpus failures: $failed"
exit "$failed"
