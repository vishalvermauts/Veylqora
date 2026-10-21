"""One pilot run (task 0, setup A) to check the wiring and the cost before the study."""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_study  # noqa: E402

record = run_study.run_one(0, "A")
keys = ("task", "setup", "passed", "test_summary", "tests_modified", "wall_seconds")
print(json.dumps({k: record[k] for k in keys}))
print(json.dumps(record["claude"])[:1200])
