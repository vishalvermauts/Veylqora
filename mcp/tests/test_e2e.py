"""End-to-end: a real Veylqora daemon, the MCP server as a subprocess, and the
MCP SDK's own client calling every tool over stdio (M2.5)."""

import asyncio
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import urllib.request

import pytest
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

HERE = pathlib.Path(__file__).resolve().parent
SERVER = HERE.parent / "veylqora_mcp" / "server.py"
FIXTURES = pathlib.Path(os.environ["VEYLQORA_FIXTURES"])
BIN = os.environ["VEYLQORA_BIN"]

EXPECTED_TOOLS = {
    "get_project_status", "search_code", "inspect_symbol", "find_dependencies",
    "search_memories", "record_memory", "inspect_evidence", "check_memory_validity",
    "get_task_context",
}


def data(result):
    """The tool result as a dict, whichever form the SDK returned it in."""
    if result.structured_content is not None:
        return result.structured_content
    return json.loads(result.content[0].text)


async def session_for(url):
    params = StdioServerParameters(
        command=sys.executable, args=[str(SERVER)], env={**os.environ, "VEYLQORA_URL": url})
    return params


def run(url, scenario):
    async def main():
        params = await session_for(url)
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                return await scenario(session)
    return asyncio.run(main())


def test_all_tools_are_discoverable(daemon):
    async def scenario(session):
        tools = await session.list_tools()
        return {t.name for t in tools.tools}
    assert run(daemon, scenario) == EXPECTED_TOOLS


def test_read_tools_return_real_data(daemon):
    async def scenario(session):
        status = await session.call_tool("get_project_status", {"root": str(FIXTURES)})
        search = await session.call_tool("search_code", {"root": str(FIXTURES), "query": "token verification", "k": 5})
        inspect = await session.call_tool("inspect_symbol", {"root": str(FIXTURES), "name": "verify_token"})
        deps = await session.call_tool("find_dependencies", {"root": str(FIXTURES), "name": "helper"})
        return status, search, inspect, deps
    status, search, inspect, deps = run(daemon, scenario)
    assert data(status)["indexed"] is True
    assert data(status)["files"] == 10
    paths = [h["path"] for h in data(search)["hits"]]
    assert "auth/tokens.py" in paths
    assert data(inspect)["definitions"][0]["path"] == "auth/tokens.py"
    assert data(deps)["dependents"] is not None


def test_memory_workflow_and_validity(daemon):
    async def scenario(session):
        created = await session.call_tool("record_memory", {
            "root": str(FIXTURES), "type": "decision", "content": "tokens are prefixed",
            "agent": "mcp-test", "evidence": [{"rel_path": "auth/tokens.py", "symbol_fqn": "auth.tokens.verify_token"}],
        })
        memory_id = data(created)["id"]
        found = await session.call_tool("search_memories", {"root": str(FIXTURES), "agent": "mcp-test", "text": "prefixed"})
        evidence = await session.call_tool("inspect_evidence", {"root": str(FIXTURES), "memory_id": memory_id, "agent": "mcp-test"})
        validity = await session.call_tool("check_memory_validity", {"root": str(FIXTURES), "memory_id": memory_id, "agent": "mcp-test"})
        return memory_id, found, evidence, validity
    memory_id, found, evidence, validity = run(daemon, scenario)
    assert any(m["id"] == memory_id for m in data(found)["memories"])
    assert data(evidence)["evidence"][0]["symbol"] == "auth.tokens.verify_token"
    assert data(validity)["valid"] is True


def test_context_package_respects_its_budget(daemon):
    async def scenario(session):
        return await session.call_tool("get_task_context", {
            "root": str(FIXTURES), "task": "token verification", "agent": "budget-test", "budget_tokens": 100})
    result = data(run(daemon, scenario))
    assert result["approx_tokens_used"] <= 100
    assert result["truncated"]["code"] >= 0


def test_invalid_requests_fail_safely(daemon):
    async def scenario(session):
        bad_k = await session.call_tool("search_code", {"root": str(FIXTURES), "query": "x", "k": 0})
        empty_query = await session.call_tool("search_code", {"root": str(FIXTURES), "query": "   "})
        return bad_k, empty_query
    bad_k, empty_query = run(daemon, scenario)
    assert bad_k.is_error
    assert empty_query.is_error


def test_refuses_a_non_loopback_daemon(daemon):
    async def scenario(session):
        return await session.call_tool("get_project_status", {"root": str(FIXTURES)})
    params = StdioServerParameters(
        command=sys.executable, args=[str(SERVER)],
        env={**os.environ, "VEYLQORA_URL": "http://10.0.0.5:8765"})

    async def main():
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                return await session.call_tool("get_project_status", {"root": str(FIXTURES)})
    result = asyncio.run(main())
    assert result.is_error
    assert "loopback" in result.content[0].text


def test_one_agent_cannot_read_another_agents_private_memory(daemon):
    async def scenario(session):
        created = await session.call_tool("record_memory", {
            "root": str(FIXTURES), "type": "observation", "content": "alice's private note",
            "agent": "alice"})
        memory_id = data(created)["id"]
        own = await session.call_tool("inspect_evidence", {
            "root": str(FIXTURES), "memory_id": memory_id, "agent": "alice"})
        other = await session.call_tool("inspect_evidence", {
            "root": str(FIXTURES), "memory_id": memory_id, "agent": "bob"})
        found = await session.call_tool("search_memories", {
            "root": str(FIXTURES), "agent": "bob", "text": "alice's private note"})
        return own, other, found
    own, other, found = run(daemon, scenario)
    assert not own.is_error
    assert other.is_error
    assert not any("private note" in m["content"] for m in data(found)["memories"])
