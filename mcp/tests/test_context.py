"""Context packages (M2.4, ADR-0016): budget, evidence, warnings, duplicates,
and recall on labelled tasks. Thresholds are fixed in the ADR."""

import asyncio
import os
import pathlib
import shutil
import sys
import tempfile

import pytest
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from conftest import FIXTURES, get_json, post_json

HERE = pathlib.Path(__file__).resolve().parent
SERVER = HERE.parent / "veylqora_mcp" / "server.py"
MIN_RECALL = 0.80  # ADR-0016

# Written from the fixture source, not from program output.
LABELLED_TASKS = [
    ("token verification", {"auth.tokens.verify_token", "auth.tokens.issue_token", "auth.tokens.TokenStore"}),
    ("password hashing", {"auth.passwords.hash_password", "auth.passwords.check_password"}),
    ("format money cents", {"billing.invoice.format_money"}),
    ("health status route", {"api.routes.health"}),
    ("slugify text", {"util.text.slugify"}),
    ("measure timing wrapper", {"util.timing.measure"}),
]


def call_context(url, task, budget):
    async def scenario():
        params = StdioServerParameters(
            command=sys.executable, args=[str(SERVER)],
            env={**os.environ, "VEYLQORA_URL": url})
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                result = await session.call_tool("get_task_context",
                                                 {"root": str(FIXTURES_ROOT), "task": task,
                                                  "budget_tokens": budget})
                return result.structured_content or __import__("json").loads(result.content[0].text)
    return asyncio.run(scenario())


FIXTURES_ROOT = FIXTURES


@pytest.mark.parametrize("budget", [100, 300, 2000])
def test_budget_is_respected(daemon, budget):
    package = call_context(daemon, "token verification", budget)
    assert package["approx_tokens_used"] <= budget


def test_no_definition_or_memory_appears_twice(daemon):
    package = call_context(daemon, "token verification", 2000)
    fqns = [h["fqn"] for h in package["sections"]["code"]]
    assert len(fqns) == len(set(fqns))
    memory_ids = [m["id"] for s in ("warnings", "memories") for m in package["sections"][s]]
    assert len(memory_ids) == len(set(memory_ids))


def test_every_evidence_reference_resolves_to_an_indexed_symbol(daemon):
    memory = post_json(daemon, "/api/v1/memories", {
        "root": str(FIXTURES_ROOT), "type": "decision", "content": "token check is prefix based",
        "author": "ctx-test",
        "evidence": [{"rel_path": "auth/tokens.py", "symbol_fqn": "auth.tokens.verify_token"}]})
    package = call_context(daemon, "token prefix", 2000)
    cited = [m for s in ("warnings", "memories") for m in package["sections"][s] if m["id"] == memory["id"]]
    assert cited, "the new memory should be offered for the task"
    for ev in cited[0]["evidence"]:
        found = get_json(daemon, "/api/v1/search?root=" + str(FIXTURES_ROOT) + "&name=" + ev["symbol"].split(".")[-1])
        assert any(h["fqn"] == ev["symbol"] and h["path"] == ev["path"] for h in found["hits"])


def test_a_memory_whose_cited_code_changed_carries_a_warning(daemon):
    work = pathlib.Path(tempfile.mkdtemp())
    src = work / "src"
    shutil.copytree(FIXTURES_ROOT, src)
    post_json(daemon, "/api/v1/index", {"root": str(src)})
    memory = post_json(daemon, "/api/v1/memories", {
        "root": str(src), "type": "decision", "content": "verify_token is the token check",
        "author": "ctx-test", "evidence": [{"rel_path": "auth/tokens.py", "symbol_fqn": "auth.tokens.verify_token"}]})
    (src / "auth" / "tokens.py").write_text(
        "def verify_token(token):\n    return bool(token)\n\n\nclass TokenStore:\n    pass\n", encoding="utf-8")
    post_json(daemon, "/api/v1/index", {"root": str(src)})

    async def scenario():
        params = StdioServerParameters(command=sys.executable, args=[str(SERVER)],
                                       env={**os.environ, "VEYLQORA_URL": daemon})
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                result = await session.call_tool("get_task_context",
                                                 {"root": str(src), "task": "verify token check"})
                return result.structured_content or __import__("json").loads(result.content[0].text)
    package = asyncio.run(scenario())
    warned = [w for w in package["sections"]["warnings"] if w["id"] == memory["id"]]
    assert warned and warned[0]["state"] == "suspected_stale"
    assert "review" in warned[0]["warning"]
    shutil.rmtree(work, ignore_errors=True)


def test_labelled_tasks_reach_the_recall_gate(daemon):
    recalls = []
    for task, relevant in LABELLED_TASKS:
        package = call_context(daemon, task, 2000)
        found = {h["fqn"] for h in package["sections"]["code"]}
        recalls.append(len(found & relevant) / len(relevant))
    mean = sum(recalls) / len(recalls)
    assert mean >= MIN_RECALL, recalls
