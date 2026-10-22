"""Blocks the shell and every other command route for all setups, limits Veylqora tools by setup,
and shows the agent only the names of the failing test files (not test names), so it has to
search the code. Verified with a no-model test: Bash, Agent, Task, WebFetch, WebSearch and
NotebookEdit are blocked together."""

import pathlib

path = pathlib.Path.home() / "study" / "run_study.py"
text = path.read_text(encoding="utf-8")


def swap(old, new, label):
    global text
    assert text.count(old) == 1, f"not found: {label}"
    text = text.replace(old, new)


# 1. Prompt: failing test files, not test names.
swap(
    '    "Failing tests ({count}):\\n{names}"\n',
    '    "Failing test files ({count}):\\n{names}"\n',
    "prompt tail",
)
swap(
    '    "Some tests in this repository fail. The failing tests are listed below. "\n',
    '    "Some tests in this repository fail. The test files that fail are listed below. "\n',
    "prompt head",
)

# 2. Per-setup tool lists: allowed tools and an explicit disallow list.
swap(
    'TOOLS = {\n    "A": BASE_TOOLS,\n    "B": BASE_TOOLS + VEYLQORA_SEARCH,\n'
    '    "C": BASE_TOOLS + VEYLQORA_SEARCH + VEYLQORA_MEMORY,\n'
    '    "D": BASE_TOOLS + VEYLQORA_SEARCH + VEYLQORA_MEMORY + VEYLQORA_CONTEXT,\n}\n',
    'TOOLS = {\n    "A": BASE_TOOLS,\n    "B": BASE_TOOLS + VEYLQORA_SEARCH,\n'
    '    "C": BASE_TOOLS + VEYLQORA_SEARCH + VEYLQORA_MEMORY,\n'
    '    "D": BASE_TOOLS + VEYLQORA_SEARCH + VEYLQORA_MEMORY + VEYLQORA_CONTEXT,\n}\n'
    '# Routes that can run a command or reach the web. Blocking Bash alone is not enough:\n'
    '# the agent can ask a subagent (Agent or Task) to run it. Verified with a no-model test.\n'
    'COMMAND_ROUTES = ["Bash", "Agent", "Task", "WebFetch", "WebSearch", "NotebookEdit"]\n'
    'DISALLOWED = {\n'
    '    "A": COMMAND_ROUTES + VEYLQORA_SEARCH + VEYLQORA_MEMORY + VEYLQORA_CONTEXT,\n'
    '    "B": COMMAND_ROUTES + VEYLQORA_MEMORY + VEYLQORA_CONTEXT,\n'
    '    "C": COMMAND_ROUTES + VEYLQORA_CONTEXT,\n'
    '    "D": COMMAND_ROUTES,\n'
    '}\n',
    "tool lists",
)

# 3. Pass the disallow list to the agent, after the allow list.
swap(
    '"--strict-mcp-config", "--allowedTools", *TOOLS[setup]]',
    '"--strict-mcp-config", "--allowedTools", *TOOLS[setup],\n'
    '                   "--disallowedTools", *DISALLOWED[setup]]',
    "command flags",
)

# 4. Failing test files instead of test IDs.
swap(
    '        names = failing[:SHOWN_FAILURES] + (["..."] if len(failing) > SHOWN_FAILURES else [])\n'
    '        prompt = PROMPT.format(count=len(failing), names="\\n".join(names))\n',
    '        files = sorted({test_id.split("::")[0] for test_id in failing})\n'
    '        names = files[:SHOWN_FAILURES] + (["..."] if len(files) > SHOWN_FAILURES else [])\n'
    '        prompt = PROMPT.format(count=len(files), names="\\n".join(names))\n',
    "prompt files",
)

path.write_text(text, encoding="utf-8")
print("tools patched")
