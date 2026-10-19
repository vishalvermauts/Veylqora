#!/usr/bin/env python3
"""Verify every certification manifest in docs/certification/.

Usage: python scripts/verify_certs.py [--strict]

Fails (exit 1) on evidence problems that are always wrong:
  * a manifest whose digest no longer matches its content (hand-edited);
  * a manifest directory that does not match the commit it names;
  * a PASS with a failed or missing mandatory check;
  * a check whose log file is missing.

Staleness is reported as a warning normally. With --strict, a module with no
PASS manifest matching the current HEAD sources also fails, which is the rule
for authorising a release (ADR-0006).
"""

import json
import pathlib
import sys

from certify import CERT_DIR, MODULES, ROOT, git, manifest_digest, module_blobs


def main(strict: bool) -> int:
    problems: list[str] = []
    warnings: list[str] = []
    head = git("rev-parse", "HEAD")
    passing_at_head: set[str] = set()
    seen_modules: set[str] = set()

    for path in sorted(CERT_DIR.glob("*/*/manifest.json")):
        where = path.relative_to(ROOT).as_posix()
        m = json.loads(path.read_text(encoding="utf-8"))
        module, commit = m.get("module", ""), m.get("commit", "")
        seen_modules.add(module)

        if manifest_digest(m) != m.get("manifest_sha256"):
            problems.append(f"{where}: content does not match manifest_sha256 (hand-edited?)")
            continue
        if path.parent.name != commit or path.parent.parent.name != module:
            problems.append(f"{where}: directory does not match module/commit")
            continue
        if module not in MODULES:
            problems.append(f"{where}: unknown module {module!r}")
            continue

        names = {c["name"] for c in m["checks"]}
        missing = set(MODULES[module]["mandatory"]) - names
        if missing:
            problems.append(f"{where}: missing mandatory checks {sorted(missing)}")

        for c in m["checks"]:
            if not (path.parent / c["log"]).is_file():
                problems.append(f"{where}: log {c['log']} is missing")

        failed = [c["name"] for c in m["checks"] if c["exit"] != 0]
        if m["status"] == "PASS" and failed:
            problems.append(f"{where}: status PASS but failed checks {failed}")

        if module_blobs(head, MODULES[module]["sources"]) == m["module_blobs"]:
            if m["status"] == "PASS":
                passing_at_head.add(module)
        else:
            warnings.append(f"{where}: stale, module sources changed since commit {commit[:12]}")

    for module in sorted(MODULES):
        if module not in passing_at_head:
            msg = f"module {module}: no PASS manifest matches HEAD sources"
            (problems if strict else warnings).append(msg)

    for w in warnings:
        print(f"WARN  {w}")
    for p in problems:
        print(f"FAIL  {p}")
    if not problems and not warnings:
        print("ok: all manifests verified")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(strict="--strict" in sys.argv[1:]))
