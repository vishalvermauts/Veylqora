"""Moves the seeded-bug list, results and raw logs into a root-only folder, so the agent's
user cannot read the answers (the old and new text of each seeded bug) or earlier results.
Run inside the sandbox once. Writes go through sudo, so the runner keeps working."""

import pathlib
import re

path = pathlib.Path.home() / "study" / "run_study.py"
text = path.read_text(encoding="utf-8")

# 1. Load the bug list from the root-only folder instead of an importable file.
old_import = (
    "sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))\n"
    "from candidates import CANDIDATES  # noqa: E402\n"
)
new_import = (
    "# The bug list is root-only (the agent's user cannot read it). Loaded through sudo.\n"
    "CANDIDATES = [tuple(item) for item in json.loads(subprocess.run(\n"
    "    [\"sudo\", \"-n\", \"cat\", \"/opt/study-private/candidates.json\"],\n"
    "    check=True, capture_output=True, text=True).stdout)]\n"
)
assert text.count(old_import) == 1, "import block not found"
text = text.replace(old_import, new_import)

# 2. Results and raw logs go to the root-only folder.
text = text.replace(
    'RESULTS = STUDY / "results-v2.jsonl"\nRAW_DIR = STUDY / "raw-v2"\nRAW_DIR.mkdir(parents=True, exist_ok=True)\n',
    'PRIVATE = pathlib.Path("/opt/study-private")  # root-only: answers and earlier results live here\n'
    'RESULTS = PRIVATE / "results-v2.jsonl"\n'
    'RAW_DIR = PRIVATE / "raw-v2"\n',
)

# 3. Helper for root-only writes, placed before the first use.
helper = (
    "def private_write(path, text, append=False):\n"
    "    \"\"\"Writes a file in the root-only folder through sudo.\"\"\"\n"
    "    args = [\"sudo\", \"-n\", \"tee\"] + ([\"-a\"] if append else []) + [str(path)]\n"
    "    subprocess.run(args, input=text, check=True, capture_output=True, text=True)\n\n\n"
)
text = text.replace("def owner():", helper + "def owner():", 1)

# 4. Use the helper for every write that must stay private.
text = text.replace(
    '        (RAW_DIR / f"{run_id}.stdout.txt").write_text(stdout, encoding="utf-8")\n'
    '        (RAW_DIR / f"{run_id}.stderr.txt").write_text(stderr, encoding="utf-8")\n',
    '        private_write(RAW_DIR / f"{run_id}.stdout.txt", stdout)\n'
    '        private_write(RAW_DIR / f"{run_id}.stderr.txt", stderr)\n',
)
text = text.replace(
    '    with RESULTS.open("a", encoding="utf-8") as out:\n'
    '        out.write(json.dumps(record) + "\\n")\n',
    '    private_write(RESULTS, json.dumps(record) + "\\n", append=True)\n',
)
text = text.replace(
    '    (RAW_DIR / f"{batch}-manifest.json").write_text(json.dumps({',
    '    private_write(RAW_DIR / f"{batch}-manifest.json", json.dumps({',
)
text = text.replace(
    '    }, indent=2), encoding="utf-8")\n\n    for task, setup in pairs:',
    '    }, indent=2))\n\n    for task, setup in pairs:',
)
# Daemon logs stay in the per-run folder (deleted after each run), not in the study folder.
text = text.replace(
    'start_daemon(run_dir / "v.db", RAW_DIR / f"{run_id}.daemon.log")',
    'start_daemon(run_dir / "v.db", run_dir / "daemon.log")',
)

path.write_text(text, encoding="utf-8")
print("runner patched")
