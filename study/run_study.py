#!/usr/bin/env python3
"""M3.6b paid agent study runner, version 2. Runs inside the CI sandbox.

Ten seeded bugs in click (pallets/click, BSD-3-Clause), each caught by click's own
test suite. Four setups, one run per (task, setup), in randomised order, on
gemini-2.5-flash through the helixgateway LiteLLM proxy with Claude Code 2.1.293.

Setups (the agent can never run commands or tests in any setup):
  A  Read, Edit, Write, Glob, Grep
  B  A + Veylqora code search tools (MCP)
  C  B + Veylqora memory tools (MCP), seeded with four generic notes
  D  C + Veylqora get_task_context (MCP)

Controls in this version:
  * The clean source lives in /opt/study-pristine/click, owned by root and not
    readable by the agent's user. Every run is copied from there.
  * The seeded bug is checked before the agent starts: the test suite must fail.
  * The agent is told which tests fail, but not the traceback or the line.
  * After the agent stops, everything outside src/click is restored from the
    pristine copy, and any difference is recorded as tests_modified. Grading uses
    the restored tree and checks that click imports from that tree.
  * The study environment has no click install, so nothing points at a source
    tree that the agent could edit.
  * Every run has its own HOME and Claude config folder, and its own process group,
    so a timeout kills the agent and its MCP server.
  * Stop reasons are recorded (subtype, errors). The study aborts on an immediate
    API failure, so a dead gateway cannot poison later rows.
  * Each row has a batch ID, a run ID and a UTC start time.
"""

import datetime
import json
import os
import pathlib
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid

# The bug list is root-only (the agent's user cannot read it). Loaded through sudo.
CANDIDATES = [tuple(item) for item in json.loads(subprocess.run(
    ["sudo", "-n", "cat", "/opt/study-private/candidates.json"],
    check=True, capture_output=True, text=True).stdout)]

HOME = pathlib.Path.home()
STUDY = HOME / "study"
PY = STUDY / "venv" / "bin" / "python"
CLAUDE = HOME / ".local" / "node_modules" / "@anthropic-ai" / "claude-code-linux-x64" / "claude"
VEYLQORA = HOME / "ci-work" / "build-gcc" / "src" / "veylqora"
MCP_SERVER = HOME / "ci" / "mcp" / "veylqora_mcp" / "server.py"
KEY_FILE = pathlib.Path("/opt/study-private/veylqora-test.key")  # root-only, read through sudo
PRISTINE = pathlib.Path("/opt/study-pristine/click")  # root-only; the agent cannot read it
FP_SCRIPT = STUDY / "fp.py"
PRIVATE = pathlib.Path("/opt/study-private")  # root-only: answers and earlier results live here
RESULTS = PRIVATE / "results-v2.jsonl"
RAW_DIR = PRIVATE / "raw-v2"

GATEWAY = "https://router.helixengine.app"
MODEL = "gemini-2.5-flash"  # gemini-3.8-flash timed out through the gateway during the study
TASK_INDEXES = [0, 2, 3, 4, 5, 6, 7, 8, 11, 12]
SETUPS = ["A", "B", "C", "D"]
RUN_TIMEOUT_S = 900
SHOWN_FAILURES = 5

PROMPT = (
    "Some tests in this repository fail. The failing tests are listed below. "
    "Find the bug in the source under src/click and fix it so that the full test suite passes. "
    "You can read and edit files, but you cannot run commands or tests. "
    "Do not modify anything outside src/click: tests, configuration and other files are checked, "
    "and any change to them counts as a failure.\n\n"
    "Failing tests ({count}):\n{names}"
)

# Generic repository notes for setups C and D. Evidence points at a definition in each
# file (the daemon needs the src.-prefixed name). None of them is the seeded bug site.
SEED_MEMORIES = [
    ("src/click/parser.py", "src.click.parser._split_opt",
     "command-line option parsing lives in src/click/parser.py; decorators in src/click/decorators.py"),
    ("src/click/types.py", "src.click.types.ParamType",
     "parameter types and validation live in src/click/types.py"),
    ("src/click/formatting.py", "src.click.formatting.HelpFormatter",
     "help formatting and text wrapping live in src/click/formatting.py and src/click/utils.py"),
    ("src/click/core.py", "src.click.core.Command",
     "Command, Option and Argument behaviour live in src/click/core.py"),
]

BASE_TOOLS = ["Read", "Edit", "Write", "Glob", "Grep"]
VEYLQORA_SEARCH = ["mcp__veylqora__get_project_status", "mcp__veylqora__search_code",
                   "mcp__veylqora__inspect_symbol", "mcp__veylqora__find_dependencies"]
VEYLQORA_MEMORY = ["mcp__veylqora__search_memories", "mcp__veylqora__record_memory",
                   "mcp__veylqora__inspect_evidence", "mcp__veylqora__check_memory_validity"]
VEYLQORA_CONTEXT = ["mcp__veylqora__get_task_context"]
TOOLS = {
    "A": BASE_TOOLS,
    "B": BASE_TOOLS + VEYLQORA_SEARCH,
    "C": BASE_TOOLS + VEYLQORA_SEARCH + VEYLQORA_MEMORY,
    "D": BASE_TOOLS + VEYLQORA_SEARCH + VEYLQORA_MEMORY + VEYLQORA_CONTEXT,
}


class StudyAbort(RuntimeError):
    """A condition that makes every later row untrustworthy. Stops the batch."""


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc)


# ---------- sandbox helpers ----------

def sudo(*args):
    return subprocess.run(["sudo", "-n", *args], check=True, capture_output=True, text=True)


def fingerprint(root, as_root=False):
    cmd = [str(PY), str(FP_SCRIPT), str(root)]
    if as_root:
        cmd = ["sudo", "-n", *cmd]
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip()


def private_write(path, text, append=False):
    """Writes a file in the root-only folder through sudo."""
    args = ["sudo", "-n", "tee"] + (["-a"] if append else []) + [str(path)]
    subprocess.run(args, input=text, check=True, capture_output=True, text=True)


def owner():
    return f"{os.getuid()}:{os.getgid()}"


def remove(path):
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def pristine_names():
    """Top-level names in the pristine copy. Listed through sudo: the agent's user cannot read it."""
    return sudo("ls", "-A", str(PRISTINE)).stdout.split()


def make_work(run_dir):
    work = run_dir / "click"
    sudo("cp", "-a", str(PRISTINE), str(work))
    sudo("chown", "-R", owner(), str(run_dir))
    return work


def restore_non_src(work):
    """Put every top-level entry except src/ back from the pristine copy. Under src/,
    only src/click is kept, because that is the code the agent may edit."""
    for entry in list(work.iterdir()):
        if entry.name != "src":
            remove(entry)
    for name in pristine_names():
        if name != "src":
            sudo("cp", "-a", str(PRISTINE / name), str(work / name))
    (work / "src").mkdir(exist_ok=True)
    for entry in list((work / "src").iterdir()):
        if entry.name != "click":
            remove(entry)
    sudo("chown", "-R", owner(), str(work))


def apply_mutation(work, path, old, new):
    target = work / path
    text = target.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise StudyAbort(f"seeded text for {path} no longer matches exactly once")
    target.write_text(text.replace(old, new), encoding="utf-8")


def verify_import(work):
    env = {**os.environ, "PYTHONPATH": str(work / "src")}
    out = subprocess.run([str(PY), "-c", "import click, sys; sys.stdout.write(click.__file__)"],
                         cwd=work, env=env, capture_output=True, text=True, timeout=60).stdout
    if not out.startswith(str(work)):
        raise StudyAbort(f"click imports from {out!r}, not from the working copy {work}")


def imports_from_copy(work):
    """True if click imports from the working copy. Used after the agent, where a broken
    import is a failed run, not a reason to stop the batch."""
    env = {**os.environ, "PYTHONPATH": str(work / "src")}
    proc = subprocess.run([str(PY), "-c", "import click, sys; sys.stdout.write(click.__file__)"],
                          cwd=work, env=env, capture_output=True, text=True, timeout=60)
    return proc.returncode == 0 and proc.stdout.startswith(str(work))


def venv_is_clean():
    """True when click cannot be imported from the study environment at all."""
    proc = subprocess.run([str(PY), "-c", "import click"], cwd="/tmp", capture_output=True, text=True)
    return proc.returncode != 0


def failing_tests(work):
    """Runs the suite on the working copy. Returns (passed, summary, failing test IDs)."""
    env = {**os.environ, "PYTHONPATH": str(work / "src")}
    try:
        proc = subprocess.run([str(PY), "-m", "pytest", "-q", "-p", "no:cacheprovider", "-rf",
                               "--no-header"],
                              cwd=work, env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT: pytest exceeded 600s", []
    lines = (proc.stdout + "\n" + proc.stderr).strip().splitlines()
    summary = lines[-1] if lines else ""
    failing = [line.split(" - ")[0][len("FAILED "):] for line in lines if line.startswith("FAILED ")]
    return proc.returncode == 0, summary, failing


# ---------- Veylqora daemon and gateway ----------

def key_value():
    return subprocess.run(["sudo", "-n", "cat", str(KEY_FILE)], check=True,
                          capture_output=True, text=True).stdout.strip()


def start_daemon(db, log_path):
    # Output goes to a file, not an unread pipe, so the daemon cannot block on a full buffer.
    log = log_path.open("w+", encoding="utf-8")
    proc = subprocess.Popen([str(VEYLQORA), "serve", "--db", str(db), "--port", "0"],
                            stdout=log, stderr=subprocess.STDOUT, text=True)
    deadline = time.monotonic() + 15
    first = ""
    while time.monotonic() < deadline and proc.poll() is None:
        log.seek(0)
        first = log.readline()
        if first.endswith("\n"):
            break
        time.sleep(0.05)
    if not first.startswith("listening on http://127.0.0.1:"):
        proc.kill()
        proc.wait(timeout=10)
        log.close()
        raise StudyAbort(f"daemon did not start: {first!r}")
    return proc, log, first.split()[2].rstrip()


def stop_daemon(proc, log):
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
    log.close()


def post(url, path, body):
    request = urllib.request.Request(url + path, data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read().decode())


def claude_env(run_dir):
    home = run_dir / "home"
    home.mkdir(exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("ANTHROPIC_")}
    env.update({
        "HOME": str(home),
        "CLAUDE_CONFIG_DIR": str(home / ".claude"),
        "ANTHROPIC_BASE_URL": GATEWAY,
        "ANTHROPIC_API_KEY": key_value(),  # as in the gateway guide, section 5A
        "CLAUDE_CODE_DISABLE_UNKNOWN_MODEL_WINDOW_ENFORCEMENT": "1",
        "CLAUDE_CODE_MAX_CONTEXT_TOKENS": "1000000",
        "DISABLE_AUTOUPDATER": "1",
    })
    return env


def parse_claude(stdout):
    if not stdout or not stdout.strip():
        return {"parse_error": True, "reason": "no output"}
    try:
        data = json.loads(stdout)
    except ValueError:
        return {"parse_error": True, "reason": "not JSON", "raw_tail": stdout[-400:]}
    if isinstance(data, list):
        item = next((x for x in reversed(data) if x.get("type") == "result"), None)
        if item is None:
            return {"parse_error": True, "reason": "no result event", "raw_tail": stdout[-400:]}
        data = item
    return {
        "is_error": bool(data.get("is_error")),
        "subtype": data.get("subtype"),
        "errors": data.get("errors"),
        "num_turns": data.get("num_turns"),
        "duration_ms": data.get("duration_ms"),
        "usage": data.get("usage"),
        "reported_cost_usd": data.get("total_cost_usd"),  # Claude-rate estimate, not real spend
        "result_tail": str(data.get("result", ""))[-300:],
    }


def run_agent(work, command, env):
    """Runs the agent in its own process group, so a timeout kills everything it started."""
    proc = subprocess.Popen(command, cwd=work, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, start_new_session=True)
    started = time.monotonic()
    try:
        stdout, stderr = proc.communicate(timeout=RUN_TIMEOUT_S)
        claude = parse_claude(stdout)
        claude["exit_code"] = proc.returncode
        if proc.returncode != 0 and stderr:
            claude["stderr_tail"] = stderr[-400:]
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        stdout, stderr = proc.communicate()
        claude = {"timeout": True, "exit_code": None}
    return claude, stdout or "", stderr or "", round(time.monotonic() - started, 1)


# ---------- one run ----------

def run_one(batch, task, setup, claude_version):
    path, old, new = CANDIDATES[task]
    run_id = f"{batch}-t{task}-{setup}-{uuid.uuid4().hex[:6]}"
    started_at = utc_now().isoformat()
    run_dir = pathlib.Path(tempfile.mkdtemp(prefix=f"study-{task}-{setup}-", dir="/tmp"))
    daemon = log = None
    try:
        work = make_work(run_dir)
        apply_mutation(work, path, old, new)
        before, _, failing = failing_tests(work)
        if before or not failing:
            raise StudyAbort(f"task {task}: the seeded fault does not make any test fail")
        verify_import(work)

        servers = {}
        if setup != "A":
            daemon, log, url = start_daemon(run_dir / "v.db", run_dir / "daemon.log")
            indexed = post(url, "/api/v1/index", {"root": str(work)})
            if indexed.get("indexed", 0) <= 0:
                raise StudyAbort(f"index returned no files: {indexed}")
            if setup in ("C", "D"):
                for rel, fqn, note in SEED_MEMORIES:
                    created = post(url, "/api/v1/memories", {
                        "root": str(work), "type": "observation", "content": note,
                        "author": "study", "scope": "shared",
                        "evidence": [{"rel_path": rel, "symbol_fqn": fqn}],
                    })
                    if "id" not in created:
                        raise StudyAbort(f"memory was not created: {created}")
            servers = {"veylqora": {"command": str(PY), "args": [str(MCP_SERVER)],
                                    "env": {"VEYLQORA_URL": url}}}
        mcp_config = run_dir / "mcp.json"
        mcp_config.write_text(json.dumps({"mcpServers": servers}), encoding="utf-8")

        names = failing[:SHOWN_FAILURES] + (["..."] if len(failing) > SHOWN_FAILURES else [])
        prompt = PROMPT.format(count=len(failing), names="\n".join(names))
        command = [str(CLAUDE), "-p", prompt, "--model", MODEL, "--output-format", "json",
                   "--dangerously-skip-permissions", "--mcp-config", str(mcp_config),
                   "--strict-mcp-config", "--allowedTools", *TOOLS[setup]]
        claude, stdout, stderr, wall = run_agent(work, command, claude_env(run_dir))
        private_write(RAW_DIR / f"{run_id}.stdout.txt", stdout)
        private_write(RAW_DIR / f"{run_id}.stderr.txt", stderr)
    finally:
        stop_daemon(daemon, log)

    # After the agent: detect and undo changes outside src/click, then grade the clean harness.
    changed_outside = fingerprint(work) != fingerprint(PRISTINE, as_root=True)
    restore_non_src(work)
    import_ok = imports_from_copy(work)
    passed, summary, _ = failing_tests(work) if import_ok else (False, "IMPORT FAILED after agent", [])
    record = {
        "run_id": run_id,
        "batch_id": batch,
        "started_at": started_at,
        "task": task,
        "file": path,
        "seeded": {"old": old, "new": new},
        "setup": setup,
        "failing_before": len(failing),
        "failing_before_examples": failing[:SHOWN_FAILURES],
        "passed": passed,
        "test_summary": summary,
        "tests_modified": changed_outside,
        "wall_seconds": wall,
        "claude": claude,
        "model_requested": MODEL,
        "claude_version": claude_version,
        "venv_clean_after_run": venv_is_clean(),
        "import_ok_after_agent": import_ok,
    }
    private_write(RESULTS, json.dumps(record) + "\n", append=True)
    shutil.rmtree(run_dir, ignore_errors=True)
    return record


def main(argv):
    tasks = [int(x) for x in argv[1:]] if len(argv) > 1 else TASK_INDEXES
    for task in tasks:
        if task not in TASK_INDEXES:
            raise SystemExit(f"task {task} is not in TASK_INDEXES {TASK_INDEXES}")
    if subprocess.run(["sudo", "-n", "test", "-d", str(PRISTINE)]).returncode != 0:
        raise SystemExit(f"pristine copy missing: {PRISTINE}")
    if not venv_is_clean():
        raise SystemExit("click is importable from the study environment; remove it first")
    pristine_fp = fingerprint(PRISTINE, as_root=True)
    version = subprocess.run([str(CLAUDE), "--version"], capture_output=True, text=True).stdout.strip()

    batch = utc_now().strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:6]
    pairs = [(t, s) for t in tasks for s in SETUPS]
    random.Random(batch).shuffle(pairs)
    private_write(RAW_DIR / f"{batch}-manifest.json", json.dumps({
        "batch_id": batch, "started_at": utc_now().isoformat(), "model": MODEL,
        "gateway": GATEWAY, "claude_version": version, "pristine_fingerprint": pristine_fp,
        "order": pairs, "run_timeout_s": RUN_TIMEOUT_S,
    }, indent=2))

    for task, setup in pairs:
        if fingerprint(PRISTINE, as_root=True) != pristine_fp:
            raise StudyAbort("the pristine copy changed during the batch")
        record = run_one(batch, task, setup, version)
        c = record["claude"]
        print(f"task {task} setup {setup}: passed={record['passed']} "
              f"tests_modified={record['tests_modified']} subtype={c.get('subtype')} "
              f"turns={c.get('num_turns')} secs={record['wall_seconds']}", flush=True)
        if not record["venv_clean_after_run"]:
            raise StudyAbort("the study environment changed during a run; stopping")
        immediate_failure = (c.get("is_error") and not c.get("timeout")
                             and (c.get("num_turns") or 0) <= 1)
        if immediate_failure:
            raise StudyAbort(f"immediate API failure on task {task} setup {setup}: {c}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except StudyAbort as stop:
        print(f"STOPPED: {stop}", file=sys.stderr)
        sys.exit(2)
