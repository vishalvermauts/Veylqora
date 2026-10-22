"""Records every tool call the agent makes, so setups B to D can be checked for real use of
the Veylqora tools. Switches Claude Code to stream-json output and counts tool_use blocks."""

import pathlib

path = pathlib.Path.home() / "study" / "run_study.py"
text = path.read_text(encoding="utf-8")

old_fmt = '"--output-format", "json",'
new_fmt = '"--output-format", "stream-json", "--verbose",'
assert text.count(old_fmt) == 1, "output format line not found"
text = text.replace(old_fmt, new_fmt)

start = text.index("def parse_claude(stdout):")
end = text.index("def run_agent(")
new_parser = '''def parse_claude(stdout):
    """Reads Claude Code's stream-json output: one JSON event per line. Returns the result
    event's fields plus every tool the agent called, by name."""
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
    for event in events:
        if event.get("type") != "assistant":
            continue
        for block in (event.get("message") or {}).get("content", []) or []:
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
        "reported_cost_usd": result.get("total_cost_usd"),  # Claude-rate estimate, not real spend
        "result_tail": str(result.get("result", ""))[-300:],
        "tool_calls": tools,
        "veylqora_tool_calls": sum(n for name, n in tools.items() if name.startswith("mcp__veylqora__")),
    }


'''
text = text[:start] + new_parser + text[end:]
path.write_text(text, encoding="utf-8")
print("tool-call logging added")
