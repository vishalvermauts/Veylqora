"""Moves the study key into the root-only folder and makes the runner read it through sudo,
so the agent's Read tool cannot open it. Run inside the sandbox."""

import pathlib
import subprocess

home = pathlib.Path.home()
subprocess.run(["sudo", "-n", "mv", str(home / ".veylqora-test.key"),
                "/opt/study-private/veylqora-test.key"], check=True)
subprocess.run(["sudo", "-n", "chmod", "600", "/opt/study-private/veylqora-test.key"], check=True)

path = home / "study" / "run_study.py"
text = path.read_text(encoding="utf-8")
old = (
    "KEY_FILE = HOME / \".veylqora-test.key\"\n"
)
new = (
    "KEY_FILE = pathlib.Path(\"/opt/study-private/veylqora-test.key\")  # root-only, read through sudo\n"
)
assert text.count(old) == 1, "key path line not found"
text = text.replace(old, new)
old_reader = (
    "def key_value():\n"
    "    return KEY_FILE.read_text(encoding=\"utf-8\").strip()\n"
)
new_reader = (
    "def key_value():\n"
    "    return subprocess.run([\"sudo\", \"-n\", \"cat\", str(KEY_FILE)], check=True,\n"
    "                          capture_output=True, text=True).stdout.strip()\n"
)
assert text.count(old_reader) == 1, "key reader not found"
text = text.replace(old_reader, new_reader)
path.write_text(text, encoding="utf-8")
print("key moved and runner patched")
