"""v0.2 acceptance, end to end over MCP: a code change marks exactly the memory that
cites the changed code as suspected stale, and no other memory (ADR-0014).

An agent records two decisions, each citing a different definition. The code of
one definition changes and is re-indexed. Checked through the MCP server with the
MCP SDK client: the affected memory is suspected stale, the unrelated one is active.
"""

import asyncio
import json
import os
import pathlib
import shutil
import sys
import tempfile

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from conftest import FIXTURES, post_json

HERE = pathlib.Path(__file__).resolve().parent
SERVER = HERE.parent / "veylqora_mcp" / "server.py"
AGENT = "scope-test"


def tool_data(result):
    if result.structured_content is not None:
        return result.structured_content
    return json.loads(result.content[0].text)


def check_validity(daemon, root, memory_ids):
    async def scenario():
        params = StdioServerParameters(command=sys.executable, args=[str(SERVER)],
                                       env={**os.environ, "VEYLQORA_URL": daemon})
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                out = {}
                for memory_id in memory_ids:
                    result = await session.call_tool("check_memory_validity", {
                        "root": root, "memory_id": memory_id, "agent": AGENT})
                    out[memory_id] = tool_data(result)
                return out

    return asyncio.run(scenario())


def test_only_the_memory_citing_changed_code_becomes_suspected_stale(daemon):
    work = pathlib.Path(tempfile.mkdtemp())
    src = work / "src"
    try:
        shutil.copytree(FIXTURES, src)
        root = str(src)
        post_json(daemon, "/api/v1/index", {"root": root})

        affected = post_json(daemon, "/api/v1/memories", {
            "root": root, "type": "decision", "content": "verify_token only checks the prefix",
            "author": AGENT,
            "evidence": [{"rel_path": "auth/tokens.py", "symbol_fqn": "auth.tokens.verify_token"}]})
        unrelated = post_json(daemon, "/api/v1/memories", {
            "root": root, "type": "decision", "content": "slugify lowercases and joins words",
            "author": AGENT,
            "evidence": [{"rel_path": "util/text.py", "symbol_fqn": "util.text.slugify"}]})

        # Only verify_token's body changes. slugify and every other file stay as they were.
        tokens = src / "auth" / "tokens.py"
        text = tokens.read_text(encoding="utf-8")
        changed = text.replace('return token.startswith("tok-")',
                               'return token.startswith("tok-") and len(token) > 4')
        assert changed != text
        tokens.write_text(changed, encoding="utf-8")
        post_json(daemon, "/api/v1/index", {"root": root})

        states = check_validity(daemon, root, [affected["id"], unrelated["id"]])
        assert states[affected["id"]]["state"] == "suspected_stale"
        assert states[affected["id"]]["valid"] is False
        assert states[unrelated["id"]]["state"] == "active"
        assert states[unrelated["id"]]["valid"] is True
    finally:
        shutil.rmtree(work, ignore_errors=True)
