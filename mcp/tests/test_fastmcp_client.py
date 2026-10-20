"""A second, independently written MCP client (M2.5 gate).

test_e2e.py drives the server with the MCP SDK's own client. This file drives
the same server with FastMCP's client, a separate implementation, and completes
a bug-investigation workflow: find the code, record what was learned with its
evidence, check that the evidence still holds, and retrieve the memory for a
later task. Passing here shows the protocol contract, not one client's quirks.
"""

import asyncio
import json
import os
import pathlib
import sys

import pytest
from fastmcp import Client
from fastmcp.client.transports import StdioTransport

HERE = pathlib.Path(__file__).resolve().parent
SERVER = HERE.parent / "veylqora_mcp" / "server.py"
FIXTURES = pathlib.Path(os.environ["VEYLQORA_FIXTURES"])

EXPECTED_TOOLS = {
    "get_project_status", "search_code", "inspect_symbol", "find_dependencies",
    "search_memories", "record_memory", "inspect_evidence", "check_memory_validity",
    "get_task_context",
}


def data(result):
    """The tool result as a dict, whichever form the client returned it in."""
    if getattr(result, "structured_content", None) is not None:
        return result.structured_content
    return json.loads(result.content[0].text)


def transport(url):
    return StdioTransport(
        command=sys.executable,
        args=[str(SERVER)],
        env={**os.environ, "VEYLQORA_URL": url},
    )


def run(url, scenario):
    async def main():
        async with Client(transport(url)) as client:
            return await scenario(client)

    return asyncio.run(main())


def test_fastmcp_client_lists_every_tool(daemon):
    async def scenario(client):
        return {tool.name for tool in await client.list_tools()}

    assert run(daemon, scenario) == EXPECTED_TOOLS


def test_fastmcp_client_completes_a_bug_investigation(daemon):
    root = str(FIXTURES)

    async def scenario(client):
        status = data(await client.call_tool("get_project_status", {"root": root}))
        found = data(await client.call_tool("inspect_symbol", {"root": root, "name": "verify_token"}))
        recorded = data(await client.call_tool("record_memory", {
            "root": root,
            "type": "observation",
            "content": "verify_token accepts the token it is given without checking its age",
            "agent": "investigator",
            "scope": "shared",
            "evidence": [{"rel_path": "auth/tokens.py", "symbol_fqn": "auth.tokens.verify_token"}],
        }))
        memory_id = recorded["id"]
        validity = data(await client.call_tool("check_memory_validity", {
            "root": root, "memory_id": memory_id, "agent": "investigator",
        }))
        evidence = data(await client.call_tool("inspect_evidence", {
            "root": root, "memory_id": memory_id, "agent": "investigator",
        }))
        context = data(await client.call_tool("get_task_context", {
            "root": root, "task": "verify_token token age", "agent": "investigator",
            "budget_tokens": 4000,
        }))
        return status, found, memory_id, validity, evidence, context

    status, found, memory_id, validity, evidence, context = run(daemon, scenario)
    assert status["indexed"] is True
    assert any(h["fqn"] == "auth.tokens.verify_token" for h in found["definitions"])
    assert validity["valid"] is True
    assert evidence["evidence"][0]["symbol"] == "auth.tokens.verify_token"
    remembered = [m["id"] for m in context["sections"]["memories"]]
    assert memory_id in remembered


def test_fastmcp_client_gets_a_protocol_error_for_a_bad_request(daemon):
    root = str(FIXTURES)

    async def scenario(client):
        with pytest.raises(Exception, match="between 1 and 100"):
            await client.call_tool("search_code", {"root": root, "query": "token", "k": 0})
        return True

    assert run(daemon, scenario) is True
