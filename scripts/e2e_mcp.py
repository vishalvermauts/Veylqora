#!/usr/bin/env python3
"""MCP connect check for the end-to-end packaging run (M3.4).

Starts the MCP server as a process, connects with the MCP SDK client, lists the
tools and calls two of them against the running daemon. Exits non-zero on any
mismatch. Expects VEYLQORA_URL and VEYLQORA_FIXTURES in the environment.
"""

import asyncio
import json
import os
import pathlib
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

SERVER = pathlib.Path(__file__).resolve().parent.parent / "mcp" / "veylqora_mcp" / "server.py"
EXPECTED_TOOLS = {
    "get_project_status", "search_code", "inspect_symbol", "find_dependencies",
    "search_memories", "record_memory", "inspect_evidence", "check_memory_validity",
    "get_task_context",
}


def data(result):
    if result.structured_content is not None:
        return result.structured_content
    return json.loads(result.content[0].text)


async def main():
    root = os.environ["VEYLQORA_FIXTURES"]
    params = StdioServerParameters(command=sys.executable, args=[str(SERVER)], env=dict(os.environ))
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = {tool.name for tool in (await session.list_tools()).tools}
            if tools != EXPECTED_TOOLS:
                raise SystemExit(f"unexpected tool list: {sorted(tools)}")
            status = data(await session.call_tool("get_project_status", {"root": root}))
            if status.get("indexed") is not True:
                raise SystemExit(f"project is not indexed: {status}")
            found = data(await session.call_tool("search_code", {"root": root, "query": "token verification", "k": 5}))
            fqns = [hit["fqn"] for hit in found["hits"]]
            if "auth.tokens.verify_token" not in fqns:
                raise SystemExit(f"search_code missed verify_token: {fqns}")
    print(f"mcp: {len(tools)} tools listed; status indexed; search_code found verify_token")


if __name__ == "__main__":
    asyncio.run(main())
