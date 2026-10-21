"""Applies each candidate fault to a fresh copy of click and records whether the
existing test suite catches it. Run inside the sandbox with the click venv."""

import json
import os
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from candidates import CANDIDATES  # noqa: E402

REPO = pathlib.Path.home() / "study" / "click"
WORK = pathlib.Path("/tmp/faults")
PYTHON = pathlib.Path.home() / "study" / "venv" / "bin" / "python"


def run_tests(root):
    # The venv installs click in editable mode, which points at the original source.
    # Put the copy's own src first, and check which click the tests really import.
    env = {**os.environ, "PYTHONPATH": str(root / "src")}
    where = subprocess.run(
        [str(PYTHON), "-c", "import click, sys; sys.stdout.write(click.__file__)"],
        cwd=root, env=env, capture_output=True, text=True, timeout=60,
    ).stdout.strip()
    if not where.startswith(str(root)):
        raise RuntimeError(f"tests would import {where}, not the copy under {root}")
    proc = subprocess.run(
        [str(PYTHON), "-m", "pytest", "-q", "-p", "no:cacheprovider", "--no-header"],
        cwd=root, env=env, capture_output=True, text=True, timeout=600,
    )
    return proc.returncode, proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""


def main():
    results = []
    for index, (path, old, new) in enumerate(CANDIDATES):
        entry = {"index": index, "file": path, "old": old, "new": new}
        target = REPO / path
        text = target.read_text(encoding="utf-8")
        count = text.count(old)
        entry["matches"] = count
        if count != 1 or old == new:
            entry["status"] = "skipped: old text must occur exactly once"
            results.append(entry)
            continue
        if WORK.exists():
            shutil.rmtree(WORK)
        shutil.copytree(REPO, WORK, ignore=shutil.ignore_patterns(".git"))
        mutated = (WORK / path).read_text(encoding="utf-8").replace(old, new)
        (WORK / path).write_text(mutated, encoding="utf-8")
        code, summary = run_tests(WORK)
        entry["caught"] = code != 0
        entry["summary"] = summary
        entry["status"] = "caught" if code != 0 else "not caught: no test fails"
        results.append(entry)
    shutil.rmtree(WORK, ignore_errors=True)
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
