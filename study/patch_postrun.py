"""After the agent finishes, a broken click import is a failed run (the agent broke the code),
not a reason to stop the batch. Only the check before a run may stop the batch."""

import pathlib

path = pathlib.Path.home() / "study" / "run_study.py"
text = path.read_text(encoding="utf-8")

old_helper = (
    "def venv_is_clean():"
)
new_helper = (
    "def imports_from_copy(work):\n"
    "    \"\"\"True if click imports from the working copy. Used after the agent, where a broken\n"
    "    import is a failed run, not a reason to stop the batch.\"\"\"\n"
    "    env = {**os.environ, \"PYTHONPATH\": str(work / \"src\")}\n"
    "    proc = subprocess.run([str(PY), \"-c\", \"import click, sys; sys.stdout.write(click.__file__)\"],\n"
    "                          cwd=work, env=env, capture_output=True, text=True, timeout=60)\n"
    "    return proc.returncode == 0 and proc.stdout.startswith(str(work))\n\n\n"
    "def venv_is_clean():"
)
assert text.count(old_helper) == 1
text = text.replace(old_helper, new_helper, 1)

old_post = (
    "    restore_non_src(work)\n"
    "    verify_import(work)\n"
    "    passed, summary, _ = failing_tests(work)\n"
)
new_post = (
    "    restore_non_src(work)\n"
    "    import_ok = imports_from_copy(work)\n"
    "    passed, summary, _ = failing_tests(work) if import_ok else (False, \"IMPORT FAILED after agent\", [])\n"
)
assert text.count(old_post) == 1, "post-run block not found"
text = text.replace(old_post, new_post)

old_field = "        \"venv_clean_after_run\": venv_is_clean(),\n"
new_field = "        \"venv_clean_after_run\": venv_is_clean(),\n        \"import_ok_after_agent\": import_ok,\n"
assert text.count(old_field) == 1
text = text.replace(old_field, new_field)

path.write_text(text, encoding="utf-8")
print("post-run import check patched")
