"""Spike S4: a real MCP client drives the stub server over stdio.

Checks, in order: initialize (protocol negotiation), tool discovery, one valid
tool call that returns real data, then the failure paths: missing required
argument and unknown tool. Prints a JSON record; exits non-zero on any failure.
"""

import asyncio
import json
import pathlib
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

SERVER = pathlib.Path(__file__).resolve().parent / "server.py"


async def run() -> dict:
    params = StdioServerParameters(command=sys.executable, args=[str(SERVER)])
    record: dict = {"checks": {}}
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            init = await session.initialize()
            record["protocol_version"] = init.protocol_version
            record["server_name"] = init.server_info.name
            record["checks"]["negotiated"] = init.protocol_version is not None

            tools = await session.list_tools()
            names = [t.name for t in tools.tools]
            record["tools"] = names
            record["checks"]["discovered"] = "search_code" in names

            call = await session.call_tool("search_code", {"query": "symbol_id"})
            payload = call.structured_content or json.loads(call.content[0].text)
            record["checks"]["valid_call_returns_data"] = (
                not call.is_error and payload["hits"][0]["symbol"] == "symbol_id"
            )

            try:
                bad = await session.call_tool("search_code", {})
                record["missing_arg"] = {"is_error": bool(bad.is_error)}
                record["checks"]["missing_arg_rejected"] = bool(bad.is_error)
            except Exception as exc:  # protocol-level error is also a safe failure
                record["missing_arg"] = {"raised": type(exc).__name__}
                record["checks"]["missing_arg_rejected"] = True

            try:
                unknown = await session.call_tool("no_such_tool", {})
                record["unknown_tool"] = {"is_error": bool(unknown.is_error)}
                record["checks"]["unknown_tool_rejected"] = bool(unknown.is_error)
            except Exception as exc:
                record["unknown_tool"] = {"raised": type(exc).__name__}
                record["checks"]["unknown_tool_rejected"] = True
    return record


if __name__ == "__main__":
    result = asyncio.run(run())
    print(json.dumps(result, indent=2))
    sys.exit(0 if all(result["checks"].values()) else 1)
