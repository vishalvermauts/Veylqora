"""Tells the agent the exact project root to pass to the Veylqora tools (the MCP tools need a
root argument; without it the agent guessed a wrong path)."""

import pathlib

path = pathlib.Path.home() / "study" / "run_study.py"
text = path.read_text(encoding="utf-8")


def swap(old, new, label):
    global text
    assert text.count(old) == 1, f"not found: {label}"
    text = text.replace(old, new)


swap(
    '    "Failing test files ({count}):\\n{names}"\n',
    '    "The project root is {root}. Pass exactly this path as the root argument of any Veylqora tool.\\n"\n'
    '    "Failing test files ({count}):\\n{names}"\n',
    "prompt root line",
)
swap(
    '        prompt = PROMPT.format(count=len(files), names="\\n".join(names))\n',
    '        prompt = PROMPT.format(root=work, count=len(files), names="\\n".join(names))\n',
    "prompt format call",
)
path.write_text(text, encoding="utf-8")
print("root line added")
