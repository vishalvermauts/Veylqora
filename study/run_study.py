#!/usr/bin/env python3
"""M3.6b paid study runner. Runs inside the CI sandbox.

Ten tasks, four setups, one run each (40 runs), on gemini-3.8-flash through the
helixgateway LiteLLM proxy. Every task is a seeded bug in click, caught by the
existing test suite (see candidates.py and find_faults.py). Each run gets a fresh
copy of click, and each run is capped with --max-budget-usd.

Setups:
  A  Claude Code, built-in file tools only (no code search, no memory)
  B  A + Veylqora code search (search_code, inspect_symbol, find_dependencies)
  C  B + Veylqora memory tools (seeded with generic repository notes)
  D  C + Veylqora get_task_context (bounded, evidence-linked context package)

The success measure is the test suite result after the agent's change. The agent
is told not to change tests, and any change to tests is recorded.
"""

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from candidates import CANDIDATES  # noqa: E402

HOME = pathlib.Path.home()
STUDY = HOME / "study"
PY = STUDY / "venv" / "bin" / "python"
CLAUDE = HOME / ".local" / "node_modules" / "@anthropic-ai" / "claude-code-linux-x64" / "claude"
VEYLQORA = HOME / "ci-work" / "build-gcc" / "src" / "veylqora"
MCP_SERVER = HOME / "ci" / "mcp" / "veylqora_mcp" / "server.py"
SOURCE = STUDY / "click"
KEY_FILE = HOME / ".veylqora-test.key"
RESULTS = STUDY / "results.jsonl"

GATEWAY = "https://router.helixengine.app"
# gemini-3.8-flash timed out through the gateway (see study notes); 2.5 Flash answers in ~4 s.
MODEL = "gemini-2.5-flash"
# Claude Code prices calls with Claude rates, so its estimate runs well above the real gemini
# spend (pilot: estimate $0.26, real $0.06). The gateway key budget ($10) is the real hard limit.
PER_RUN_BUDGET_USD = "1.00"
TASK_INDEXES = [0, 2, 3, 4, 5, 6, 7, 8, 11, 12]
SETUPS = ["A", "B", "C", "D"]

PROMPT = (
    "The test suite in this repository fails: a bug was introduced in the source. "
    "Find the bug in src/click and fix the source so that the full test suite passes. "
    "Do not modify anything under tests/. Run the test suite to confirm, then stop."
)

SEED_MEMORIES = [
    "click: command-line option parsing lives in src/click/parser.py; decorators in src/click/decorators.py",
    "click: parameter types and validation live in src/click/types.py",
    "click: help formatting and text wrapping live in src/click/formatting.py and src/click/utils.py",
    "click: Command, Option and Argument behaviour live in src/click/core.py",
]

BASE_TOOLS = ["Read", "Edit", "Write", "Bash", "Glob", "Grep"]
VEYLQORA_SEARCH = [
    "mcp__veylqora__get_project_status",
    "mcp__veylqora__search_code",
    "mcp__veylqora__inspect_symbol",
    "mcp__veylqora__find_dependencies",
]
VEYLQORA_MEMORY = [
    "mcp__veylqora__search_memories",
    "mcp__veylqora__record_memory",
    "mcp__veylqora__inspect_evidence",
    "mcp__veylqora__check_memory_validity",
]
VEYLQORA_CONTEXT = ["mcp__veylqora__get_task_context"]
TOOLS = {
    "A": BASE_TOOLS,
    "B": BASE_TOOLS + VEYLQORA_SEARCH,
    "C": BASE_TOOLS + VEYLQORA_SEARCH + VEYLQORA_MEMORY,
    "D": BASE_TOOLS + VEYLQORA_SEARCH + VEYLQORA_MEMORY + VEYLQORA_CONTEXT,
}


def key_value():
    return KEY_FILE.read_text(encoding="utf-8").strip()


def claude_env():
    env = {k: v for k, v in os.environ.items() if not k.startswith("ANTHROPIC_")}
    env.update({
        "ANTHROPIC_BASE_URL": GATEWAY,
        "ANTHROPIC_API_KEY": key_value(),  # as in the gateway guide, section 5A
        "CLAUDE_CODE_DISABLE_UNKNOWN_MODEL_WINDOW_ENFORCEMENT": "1",
        "CLAUDE_CODE_MAX_CONTEXT_TOKENS": "1000000",
        "DISABLE_AUTOUPDATER": "1",
    })
    return env


def start_daemon(db):
    proc = subprocess.Popen([str(VEYLQORA), "serve", "--db", str(db), "--port", "0"],
                            stdout=subprocess.PIPE, text=True)
    line = proc.stdout.readline()
    if not line.startswith("listening on http://127.0.0.1:"):
        proc.kill()
        raise RuntimeError(f"daemon did not start: {line!r}")
    return proc, line.split()[2].rstrip()


def post(url, path, body):
    request = urllib.request.Request(url + path, data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read().decode())


def tests_fingerprint(root):
    digest = hashlib.sha256()
    for path in sorted((root / "tests").rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts:
            digest.update(str(path.relative_to(root)).encode())
            digest.update(path.read_bytes())
    return digest.hexdigest()


def run_tests(work):
    env = {**os.environ, "PYTHONPATH": str(work / "src")}
    proc = subprocess.run([str(PY), "-m", "pytest", "-q", "-p", "no:cacheprovider", "--no-header"],
                          cwd=work, env=env, capture_output=True, text=True, timeout=600)
    summary = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
    return proc.returncode == 0, summary


def parse_claude(stdout):
    try:
        data = json.loads(stdout)
    except (ValueError, TypeError):
        return {"parse_error": True, "raw_tail": stdout[-400:]}
    if isinstance(data, list):
        data = next((item for item in reversed(data) if item.get("type") == "result"), {})
    return {
        "is_error": bool(data.get("is_error")),
        "num_turns": data.get("num_turns"),
        "duration_ms": data.get("duration_ms"),
        "usage": data.get("usage"),
        "reported_cost_usd": data.get("total_cost_usd"),
        "result_tail": str(data.get("result", ""))[-300:],
    }


def run_one(task_index, setup):
    path, old, new = CANDIDATES[task_index]
    run_dir = pathlib.Path(tempfile.mkdtemp(prefix=f"study-t{task_index}-{setup}-", dir="/tmp"))
    work = run_dir / "click"
    shutil.copytree(SOURCE, work, ignore=shutil.ignore_patterns(".git", "__pycache__"))
    target = work / path
    text = target.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise RuntimeError(f"task {task_index}: the seeded line no longer matches exactly once")
    target.write_text(text.replace(old, new), encoding="utf-8")
    before = tests_fingerprint(work)

    daemon = None
    servers = {}
    if setup != "A":
        daemon, url = start_daemon(run_dir / "v.db")
        post(url, "/api/v1/index", {"root": str(work)})
        if setup in ("C", "D"):
            for note in SEED_MEMORIES:
                post(url, "/api/v1/memories", {"root": str(work), "type": "observation",
                                               "content": note, "author": "study", "scope": "shared"})
        servers = {"veylqora": {"command": str(PY), "args": [str(MCP_SERVER)],
                                "env": {"VEYLQORA_URL": url}}}
    mcp_config = run_dir / "mcp.json"
    mcp_config.write_text(json.dumps({"mcpServers": servers}), encoding="utf-8")

    command = [str(CLAUDE), "-p", PROMPT, "--model", MODEL, "--output-format", "json",
               "--max-budget-usd", PER_RUN_BUDGET_USD, "--dangerously-skip-permissions",
               "--mcp-config", str(mcp_config), "--strict-mcp-config", "--allowedTools", *TOOLS[setup]]
    started = time.time()
    try:
        proc = subprocess.run(command, cwd=work, env=claude_env(), capture_output=True, text=True,
                              timeout=900)
        claude = parse_claude(proc.stdout)
        claude["exit_code"] = proc.returncode
        if proc.returncode != 0 and proc.stderr:
            claude["stderr_tail"] = proc.stderr[-400:]
    except subprocess.TimeoutExpired:
        claude = {"timeout": True}
    finally:
        if daemon is not None:
            daemon.terminate()
            daemon.wait(timeout=20)

    passed, summary = run_tests(work)
    record = {
        "task": task_index,
        "file": path,
        "setup": setup,
        "passed": passed,
        "test_summary": summary,
        "tests_modified": tests_fingerprint(work) != before,
        "wall_seconds": round(time.time() - started, 1),
        "claude": claude,
        "model": MODEL,
    }
    with RESULTS.open("a", encoding="utf-8") as out:
        out.write(json.dumps(record) + "\n")
    shutil.rmtree(run_dir, ignore_errors=True)
    return record


def main(argv):
    tasks = [int(x) for x in argv[1:]] if len(argv) > 1 else TASK_INDEXES
    for task in tasks:
        for setup in SETUPS:
            record = run_one(task, setup)
            print(f"task {task} setup {setup}: passed={record['passed']} "
                  f"tests_modified={record['tests_modified']} summary={record['test_summary'][-60:]}",
                  flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
