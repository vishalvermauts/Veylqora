"""Dry test of the v2 harness with no model calls. A stand-in agent replaces the
real one, so this costs nothing. It checks:
  1. a correct edit inside src/click is graded as a pass, with tests_modified false;
  2. an edit to tests/ is detected (tests_modified true) and is not allowed to pass;
  3. the pristine copy is not readable by the agent's user;
  4. the seeded fault makes the suite fail before the agent starts.
"""

import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run_study as study  # noqa: E402

TASK = 0
path, old, new = study.CANDIDATES[TASK]
MODE = sys.argv[1] if len(sys.argv) > 1 else "fix"


def fake_agent(work, command, env):
    if MODE == "fix":
        target = work / path
        target.write_text(target.read_text(encoding="utf-8").replace(new, old), encoding="utf-8")
    elif MODE == "tamper":
        test = next((work / "tests").rglob("test_make_default_short_help.py"))
        test.write_text("def test_make_default_short_help():\n    pass\n", encoding="utf-8")
    return {"timeout": False, "is_error": False, "num_turns": 3, "subtype": "success"}, "", "", 0.0


study.run_agent = fake_agent
study.RESULTS = study.PRIVATE / "results-dry-v3.jsonl"
SETUP = sys.argv[2] if len(sys.argv) > 2 else "A"
record = study.run_one("dry", TASK, SETUP, "dry-test")
print(json.dumps({k: record[k] for k in ("passed", "tests_modified", "failing_before", "test_summary")}, indent=1))

