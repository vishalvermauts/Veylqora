"""Three fixes from the 8-run check:
1. A run that hit the time limit is never a pass, even if the tests pass afterwards.
2. The agent is told to stop once it believes the fix is complete (it cannot run tests).
3. Leftover veylqora daemons from earlier runs are stopped when a batch starts."""

import pathlib

path = pathlib.Path.home() / "study" / "run_study.py"
text = path.read_text(encoding="utf-8")


def swap(old, new, label):
    global text
    assert text.count(old) == 1, f"not found: {label}"
    text = text.replace(old, new)


# 1. Timeouts are not passes.
swap(
    '        "passed": passed,\n',
    '        "passed": passed and not claude.get("timeout"),\n'
    '        "raw_tests_passed_after_run": passed,\n'
    '        "timed_out": bool(claude.get("timeout")),\n',
    "passed field",
)

# 2. Tell the agent when to stop.
swap(
    '    "You can read and edit files, but you cannot run commands or tests. "\n',
    '    "You can read and edit files, but you cannot run commands or tests. "\n'
    '    "When you believe the fix is complete, stop and reply with a short summary; "\n'
    '    "do not keep re-checking the code. "\n',
    "prompt stop rule",
)

# 3. Clear leftover daemons when a batch starts.
swap(
    "def main(argv):\n",
    "def stop_leftover_daemons():\n"
    "    \"\"\"Stops veylqora daemons left running by earlier runs (a run that was killed by hand\n"
    "    never reaches its cleanup step). Called once at the start of a batch.\"\"\"\n"
    "    stopped = []\n"
    "    for proc in pathlib.Path(\"/proc\").iterdir():\n"
    "        if not proc.name.isdigit():\n"
    "            continue\n"
    "        try:\n"
    "            cmd = (proc / \"cmdline\").read_bytes().replace(b\"\\0\", b\" \").decode(errors=\"replace\")\n"
    "        except OSError:\n"
    "            continue\n"
    "        if \"/build-gcc/src/veylqora serve\" in cmd:\n"
    "            os.kill(int(proc.name), signal.SIGTERM)\n"
    "            stopped.append(proc.name)\n"
    "    return stopped\n\n\n"
    "def main(argv):\n",
    "main def",
)
swap(
    "    pristine_fp = fingerprint(PRISTINE, as_root=True)\n",
    "    leftovers = stop_leftover_daemons()\n"
    "    if leftovers:\n"
    "        print(f\"stopped {len(leftovers)} leftover daemon(s): {leftovers}\", flush=True)\n"
    "    pristine_fp = fingerprint(PRISTINE, as_root=True)\n",
    "main daemon call",
)

path.write_text(text, encoding="utf-8")
print("fixes applied")
