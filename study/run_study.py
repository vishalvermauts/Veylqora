#!/usr/bin/env python3
"""M3.6b paid agent study runner, version 3. Runs inside the CI sandbox.

Ten seeded bugs in click (pallets/click, BSD-3-Clause), each caught by click's own test
suite. Four setups, one run per (task, setup), in randomised order, on gemini-2.5-flash
through the helixgateway LiteLLM proxy with the agent CLI 2.1.293.

Setups (the agent can never run commands, tests or subagents in any setup):
  A  Read, Edit, Write, Glob, Grep
  B  A + Veylqora code search tools (MCP)
  C  B + Veylqora memory tools (MCP), seeded with four generic notes
  D  C + Veylqora get_task_context (MCP)

Changes from version 2 (see study/README.md for the full list):
  * The prompt gives a symptom in words, not the names of the failing test files.
  * Tool names in the hints are the real MCP names (mcp__veylqora__...).
  * The API key reaches the agent CLI through apiKeyHelper, not an environment variable.
  * Transient failures are retried (up to MAX_ATTEMPTS, each on a fresh copy). A row that
    still fails after the retries is an infrastructure error, recorded but not scored.
  * The served model is taken from the stream, and cost is computed from the usage at the
    gateway's published prices. The agent CLI's own cost figure is kept only for reference.
  * Each run's temporary folder is removed, including on abort.

Controls (unchanged from version 2):
  * The clean source lives in /opt/study-pristine/click, owned by root and unreadable by
    the agent. Every run is copied from there.
  * The seeded bug is checked before the agent starts: the test suite must fail.
  * After the agent stops, everything outside src/click is restored from the pristine copy,
    and any difference is recorded as tests_modified. Grading uses the restored tree and
    checks that click imports from that tree.
  * The study environment has no click install, so nothing points at a source tree the
    agent could edit.
  * Every run has its own HOME and agent config folder and its own process group, so a
    timeout kills the agent and its MCP server.
  * The study aborts on an immediate API failure that survives the retries.
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
AGENT_CLI = pathlib.Path(os.environ["STUDY_AGENT_CLI"])  # set in the sandbox; see study/README.md
VEYLQORA = HOME / "ci-work" / "build-gcc" / "src" / "veylqora"
MCP_SERVER = HOME / "ci" / "mcp" / "veylqora_mcp" / "server.py"
KEY_FILE = pathlib.Path("/opt/study-private/veylqora-study-2.key")  # root-only, read through sudo
PRISTINE = pathlib.Path("/opt/study-pristine/click")  # root-only; the agent cannot read it
FP_SCRIPT = STUDY / "fp.py"
PRIVATE = pathlib.Path("/opt/study-private")  # root-only: answers and earlier results live here
RAW_DIR = PRIVATE / "raw-v3"
RESULTS_FULL = PRIVATE / "results-v3.jsonl"
RESULTS_PILOT = PRIVATE / "results-pilot-v3.jsonl"
RESULTS = RESULTS_FULL  # switched to RESULTS_PILOT by the pilot mode

GATEWAY = "https://router.helixengine.app"
MODEL = "gemini-2.5-flash"  # gemini-3.8-flash timed out through the gateway during the study
# Published gateway prices, USD per million tokens (from /model/info).
PRICE_INPUT, PRICE_CACHE_READ, PRICE_OUTPUT = 0.30, 0.03, 2.50

TASK_INDEXES = [0, 2, 3, 4, 5, 6, 7, 8, 11, 12]
PART_1 = [0, 2, 3, 4, 5]
PART_2 = [6, 7, 8, 11, 12]
PILOT_PAIRS = [(0, "A"), (2, "B"), (3, "C"), (4, "D"), (5, "A"), (6, "B"), (7, "C"), (8, "D")]
SETUPS = ["A", "B", "C", "D"]
RUN_TIMEOUT_S = 900
MAX_ATTEMPTS = 3
VEYLQORA_AGENT = "study-agent"

# What the reporter saw, per task. Written from the seeded change, without naming files or lines.
SYMPTOMS = {
    0: "A string whose length equals the limit is shortened, when it should be returned unchanged.",
    2: "Text handling gives the wrong output for some inputs, including the empty case.",
    3: "Output is shortened or left whole the wrong way round, depending on the shorten setting.",
    4: "A range-limited number with clamping switched on is not clamped, and with it switched off it is.",
    5: "An integer range with only a minimum set accepts and rejects the wrong values.",
    6: "An integer range with only a maximum set accepts and rejects the wrong values.",
    7: "Choice matching ignores or applies letter case the wrong way round.",
    8: "An argument declared with an unlimited count does not accept several values.",
    11: "A required option that is missing is not reported, and an optional one is reported as missing.",
    12: "A boolean flag takes the wrong default when no value is given.",
}

PROMPT = (
    "A bug has been introduced into the click library. The reported symptom is: {symptom}\n"
    "{count} test(s) fail. Find the bug in the source under src/click and fix it so that the "
    "full test suite passes. "
    "You can read and edit files, but you cannot run commands or tests. "
    "When you believe the fix is complete, stop and reply with a short summary; "
    "do not keep re-checking the code. "
    "Do not modify anything outside src/click: tests, configuration and other files are checked, "
    "and any change to them counts as a failure.\n\n"
    "The project root is {root}. Pass exactly this path as the root argument of any Veylqora tool."
)

# Added to the prompt for setups that have Veylqora tools. Setup A has none.
TOOL_HINT = {
    "B": "Use the mcp__veylqora__search_code tool (with the root above) to locate the relevant code "
         "before you edit. mcp__veylqora__inspect_symbol and mcp__veylqora__find_dependencies show what "
         "a definition calls and what calls it.",
    "C": "Use the mcp__veylqora__search_code tool (with the root above) to locate the relevant code "
         "before you edit. Check the Veylqora memory notes with mcp__veylqora__search_memories. "
         "mcp__veylqora__inspect_symbol and mcp__veylqora__find_dependencies show what a definition "
         "calls and what calls it.",
    "D": "Start with the mcp__veylqora__get_task_context tool (with the root above) for a bounded "
         "context package for this task. Then use mcp__veylqora__search_code to locate the relevant "
         "code before you edit. Check the memory notes with mcp__veylqora__search_memories and "
         "mcp__veylqora__check_memory_validity.",
}

# Generic repository notes for setups C and D. Evidence points at a definition in each
# file (the daemon needs the src.-prefixed name). None of them is a seeded bug site.
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
# Routes that can run a command or reach the web. Blocking Bash alone is not enough:
# the agent can ask a subagent (Agent or Task) to run it. Verified with a no-model test.
COMMAND_ROUTES = ["Bash", "Agent", "Task", "WebFetch", "WebSearch", "NotebookEdit"]
DISALLOWED = {
    "A": COMMAND_ROUTES + VEYLQORA_SEARCH + VEYLQORA_MEMORY + VEYLQORA_CONTEXT,
    "B": COMMAND_ROUTES + VEYLQORA_MEMORY + VEYLQORA_CONTEXT,
    "C": COMMAND_ROUTES + VEYLQORA_CONTEXT,
    "D": COMMAND_ROUTES,
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


def make_work(attempt_dir):
    work = attempt_dir / "click"
    sudo("cp", "-a", str(PRISTINE), str(work))
    sudo("chown", "-R", owner(), str(attempt_dir))
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


def agent_env(attempt_dir):
    """Environment and settings for one agent run. The key is not in the environment: the agent CLI
    calls the helper script, which reads the root-only key file through sudo."""
    home = attempt_dir / "home"
    config = home / "agent-config"
    config.mkdir(parents=True, exist_ok=True)
    helper = attempt_dir / "key.sh"
    helper.write_text(f"#!/bin/sh\nexec sudo -n cat {KEY_FILE}\n", encoding="utf-8")
    helper.chmod(0o700)
    (config / "settings.json").write_text(json.dumps({"apiKeyHelper": str(helper)}), encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if not k.startswith("ANTHROPIC_")}
    env.update({
        "HOME": str(home),
        "ANTHROPIC_BASE_URL": GATEWAY,
        "CLAUDE_CODE_DISABLE_UNKNOWN_MODEL_WINDOW_ENFORCEMENT": "1",
        "CLAUDE_CODE_MAX_CONTEXT_TOKENS": "1000000",
        "DISABLE_AUTOUPDATER": "1",
    })
    return env


def gateway_cost(usage):
    """Cost in USD at the gateway's published prices. Cache writes are charged at the input rate
    (an assumption: the gateway's price for cache writes was not checked)."""
    usage = usage or {}

    def tokens(key):
        return int(usage.get(key) or 0)

    return round((tokens("input_tokens") + tokens("cache_creation_input_tokens")) * PRICE_INPUT / 1e6
                 + tokens("cache_read_input_tokens") * PRICE_CACHE_READ / 1e6
                 + tokens("output_tokens") * PRICE_OUTPUT / 1e6, 6)


def parse_agent_output(stdout):
    """Reads the agent CLI's stream-json output: one JSON event per line. Returns the result
    event's fields, every tool the agent called by name, and the models the stream reports."""
    if not stdout or not stdout.strip():
        return {"parse_error": True, "reason": "no output"}
    events = []
    for line in stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            events.append(json.loads(line))
        except ValueError:
            continue
    result = next((e for e in reversed(events) if e.get("type") == "result"), None)
    if result is None:
        return {"parse_error": True, "reason": "no result event", "raw_tail": stdout[-400:]}
    tools = {}
    models = set()
    init = next((e for e in events if e.get("type") == "system" and e.get("subtype") == "init"), None)
    if init and init.get("model"):
        models.add(init["model"])
    for event in events:
        if event.get("type") != "assistant":
            continue
        message = event.get("message") or {}
        if message.get("model"):
            models.add(message["model"])
        for block in message.get("content", []) or []:
            if isinstance(block, dict) and block.get("type") == "tool_use":
                name = block.get("name", "?")
                tools[name] = tools.get(name, 0) + 1
    return {
        "is_error": bool(result.get("is_error")),
        "subtype": result.get("subtype"),
        "errors": result.get("errors"),
        "num_turns": result.get("num_turns"),
        "duration_ms": result.get("duration_ms"),
        "usage": result.get("usage"),
        "gateway_cost_usd": gateway_cost(result.get("usage")),
        "vendor_cost_usd": result.get("total_cost_usd"),  # vendor-rate figure, reference only
        "served_models": sorted(models),
        "result_tail": str(result.get("result", ""))[-300:],
        "tool_calls": tools,
        "veylqora_tool_calls": sum(n for name, n in tools.items() if name.startswith("mcp__veylqora__")),
    }


def run_agent(work, command, env):
    """Runs the agent in its own process group, so a timeout kills everything it started."""
    proc = subprocess.Popen(command, cwd=work, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, start_new_session=True)
    started = time.monotonic()
    try:
        stdout, stderr = proc.communicate(timeout=RUN_TIMEOUT_S)
        agent = parse_agent_output(stdout)
        agent["exit_code"] = proc.returncode
        if proc.returncode != 0 and stderr:
            agent["stderr_tail"] = stderr[-400:]
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        stdout, stderr = proc.communicate()
        agent = {"timeout": True, "exit_code": None}
    return agent, stdout or "", stderr or "", round(time.monotonic() - started, 1)


def transient(agent):
    """A failure that comes from the gateway or the client, not from the agent's work: no
    output, no result event, or an error with at most one turn. Timeouts are not transient:
    they are recorded as a failed run."""
    if agent.get("timeout"):
        return False
    if agent.get("parse_error"):
        return True
    return bool(agent.get("is_error")) and (agent.get("num_turns") or 0) <= 1


# ---------- one run ----------

def _attempt(batch, task, setup, agent_version, attempt, attempt_dir):
    path, old, new = CANDIDATES[task]
    run_id = f"{batch}-t{task}-{setup}-a{attempt}-{uuid.uuid4().hex[:6]}"
    started_at = utc_now().isoformat()
    daemon = log = None
    try:
        work = make_work(attempt_dir)
        apply_mutation(work, path, old, new)
        before, _, failing = failing_tests(work)
        if before or not failing:
            raise StudyAbort(f"task {task}: the seeded fault does not make any test fail")
        verify_import(work)

        servers = {}
        if setup != "A":
            daemon, log, url = start_daemon(attempt_dir / "v.db", attempt_dir / "daemon.log")
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
                                    "env": {"VEYLQORA_URL": url, "VEYLQORA_DEFAULT_ROOT": str(work),
                                            "VEYLQORA_AGENT": VEYLQORA_AGENT}}}
        mcp_config = attempt_dir / "mcp.json"
        mcp_config.write_text(json.dumps({"mcpServers": servers}), encoding="utf-8")

        prompt = PROMPT.format(symptom=SYMPTOMS[task], root=work, count=len(failing))
        if setup in TOOL_HINT:
            prompt += "\n\n" + TOOL_HINT[setup]
        command = [str(AGENT_CLI), "-p", prompt, "--model", MODEL, "--output-format", "stream-json",
                   "--verbose", "--dangerously-skip-permissions", "--mcp-config", str(mcp_config),
                   "--strict-mcp-config", "--allowedTools", *TOOLS[setup],
                   "--disallowedTools", *DISALLOWED[setup]]
        agent, stdout, stderr, wall = run_agent(work, command, agent_env(attempt_dir))
        private_write(RAW_DIR / f"{run_id}.stdout.txt", stdout)
        private_write(RAW_DIR / f"{run_id}.stderr.txt", stderr)
    finally:
        stop_daemon(daemon, log)

    # After the agent: detect and undo changes outside src/click, then grade the clean harness.
    changed_outside = fingerprint(work) != fingerprint(PRISTINE, as_root=True)
    restore_non_src(work)
    import_ok = imports_from_copy(work)
    passed, summary, _ = failing_tests(work) if import_ok else (False, "IMPORT FAILED after agent", [])
    infra = transient(agent)
    return {
        "run_id": run_id,
        "batch_id": batch,
        "attempt": attempt,
        "started_at": started_at,
        "task": task,
        "file": path,
        "seeded": {"old": old, "new": new},
        "symptom": SYMPTOMS[task],
        "setup": setup,
        "failing_before": len(failing),
        "failing_before_examples": failing[:5],
        "infra_error": infra,
        "passed": None if infra else (passed and not agent.get("timeout")),
        "raw_tests_passed_after_run": passed,
        "timed_out": bool(agent.get("timeout")),
        "test_summary": summary,
        "tests_modified": changed_outside,
        "wall_seconds": wall,
        "agent": agent,
        "model_requested": MODEL,
        "agent_version": agent_version,
        "venv_clean_after_run": venv_is_clean(),
        "import_ok_after_agent": import_ok,
    }


def run_one(batch, task, setup, agent_version):
    """One row: up to MAX_ATTEMPTS attempts, each on a fresh copy. Only the last attempt is
    kept as the row. Earlier transient attempts are listed in row['attempts']."""
    run_dir = pathlib.Path(tempfile.mkdtemp(prefix=f"study-{task}-{setup}-", dir="/tmp"))
    attempts = []
    try:
        for attempt in range(1, MAX_ATTEMPTS + 1):
            attempt_dir = run_dir / f"attempt-{attempt}"
            attempt_dir.mkdir()
            record = _attempt(batch, task, setup, agent_version, attempt, attempt_dir)
            attempts.append({"attempt": attempt, "infra_error": record["infra_error"],
                             "subtype": record["agent"].get("subtype"),
                             "num_turns": record["agent"].get("num_turns")})
            if not record["infra_error"]:
                break
        record["attempts"] = attempts
        private_write(RESULTS, json.dumps(record) + "\n", append=True)
        return record
    finally:
        shutil.rmtree(run_dir, ignore_errors=True)


def stop_leftover_daemons():
    """Stops veylqora daemons left running by earlier runs (a run that was killed by hand
    never reaches its cleanup step). Called once at the start of a batch."""
    stopped = []
    for proc in pathlib.Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            cmd = (proc / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        if "/build-gcc/src/veylqora serve" in cmd:
            os.kill(int(proc.name), signal.SIGTERM)
            stopped.append(proc.name)
    return stopped


def main(argv):
    global RESULTS
    mode = argv[1] if len(argv) > 1 else "full"
    if mode == "pilot":
        pairs_in = PILOT_PAIRS
        RESULTS = RESULTS_PILOT
    elif mode in ("part1", "part2"):
        pairs_in = [(t, s) for t in (PART_1 if mode == "part1" else PART_2) for s in SETUPS]
    elif mode == "full":
        pairs_in = [(t, s) for t in TASK_INDEXES for s in SETUPS]
    else:
        raise SystemExit("usage: run_study.py pilot | part1 | part2 | full")
    if subprocess.run(["sudo", "-n", "test", "-d", str(PRISTINE)]).returncode != 0:
        raise SystemExit(f"pristine copy missing: {PRISTINE}")
    if not venv_is_clean():
        raise SystemExit("click is importable from the study environment; remove it first")
    leftovers = stop_leftover_daemons()
    if leftovers:
        print(f"stopped {len(leftovers)} leftover daemon(s): {leftovers}", flush=True)
    sudo("mkdir", "-p", str(RAW_DIR))
    pristine_fp = fingerprint(PRISTINE, as_root=True)
    version = subprocess.run([str(AGENT_CLI), "--version"], capture_output=True, text=True).stdout.strip()

    batch = utc_now().strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:6]
    pairs = list(pairs_in)
    random.Random(batch).shuffle(pairs)
    private_write(RAW_DIR / f"{batch}-manifest.json", json.dumps({
        "batch_id": batch, "mode": mode, "started_at": utc_now().isoformat(), "model": MODEL,
        "gateway": GATEWAY, "agent_version": version, "pristine_fingerprint": pristine_fp,
        "order": pairs, "run_timeout_s": RUN_TIMEOUT_S, "max_attempts": MAX_ATTEMPTS,
        "results_file": str(RESULTS),
    }, indent=2))

    for task, setup in pairs:
        if fingerprint(PRISTINE, as_root=True) != pristine_fp:
            raise StudyAbort("the pristine copy changed during the batch")
        record = run_one(batch, task, setup, version)
        c = record["agent"]
        print(f"task {task} setup {setup}: passed={record['passed']} infra_error={record['infra_error']} "
              f"attempts={len(record['attempts'])} tests_modified={record['tests_modified']} "
              f"subtype={c.get('subtype')} turns={c.get('num_turns')} "
              f"veylqora_calls={c.get('veylqora_tool_calls')} served={c.get('served_models')} "
              f"cost=${c.get('gateway_cost_usd')} secs={record['wall_seconds']}", flush=True)
        if not record["venv_clean_after_run"]:
            raise StudyAbort("the study environment changed during a run; stopping")
        if record["infra_error"]:
            raise StudyAbort(f"gateway or client failed {MAX_ATTEMPTS} times on task {task} setup {setup}: {c}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except StudyAbort as stop:
        print(f"STOPPED: {stop}", file=sys.stderr)
        sys.exit(2)
