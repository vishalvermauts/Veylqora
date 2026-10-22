"""Setups B to D are told to use the Veylqora tools they have. Setup A has no Veylqora tools and
gets no such line. Without this, the tools were available but never called."""

import pathlib

path = pathlib.Path.home() / "study" / "run_study.py"
text = path.read_text(encoding="utf-8")


def swap(old, new, label):
    global text
    assert text.count(old) == 1, f"not found: {label}"
    text = text.replace(old, new)


swap(
    "BASE_TOOLS = [",
    "# Added to the prompt for setups that have Veylqora tools (B, C, D). Setup A has none.\n"
    "TOOL_HINT = {\n"
    "    \"B\": \"Use the Veylqora search_code tool (with the root above) to locate the relevant code \"\n"
    "          \"before you edit. inspect_symbol and find_dependencies show what a definition calls and what calls it.\",\n"
    "    \"C\": \"Use the Veylqora search_code tool (with the root above) to locate the relevant code \"\n"
    "          \"before you edit. Check the Veylqora memory notes with search_memories. \"\n"
    "          \"inspect_symbol and find_dependencies show what a definition calls and what calls it.\",\n"
    "    \"D\": \"Start with the Veylqora get_task_context tool (with the root above) for a bounded context package \"\n"
    "          \"for this task. Then use search_code to locate the relevant code before you edit. \"\n"
    "          \"Check the memory notes with search_memories and check_memory_validity.\",\n"
    "}\n\n"
    "BASE_TOOLS = [",
    "tool hints",
)
swap(
    '        prompt = PROMPT.format(root=work, count=len(files), names="\\n".join(names))\n',
    '        prompt = PROMPT.format(root=work, count=len(files), names="\\n".join(names))\n'
    '        if setup in TOOL_HINT:\n'
    '            prompt += "\\n\\n" + TOOL_HINT[setup]\n',
    "prompt hint",
)
path.write_text(text, encoding="utf-8")
print("hints added")
