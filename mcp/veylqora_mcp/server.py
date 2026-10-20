"""Model Context Protocol server for Veylqora (M2.5, ADR-0007).

A thin process: it turns MCP tool calls into requests to the local Veylqora
daemon and returns the daemon's answers unchanged. It stores nothing itself.
The daemon must be on a loopback address; any other address is refused.
"""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError as SdkToolError

LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}

# A rough token estimate for context budgeting: about four characters per token.
CHARS_PER_TOKEN = 4


class ToolError(SdkToolError):
    """A request the daemon refused, or an argument this server rejects."""


def daemon_url() -> str:
    url = os.environ.get("VEYLQORA_URL", "http://127.0.0.1:8765")
    host = urllib.parse.urlparse(url).hostname or ""
    if host not in LOOPBACK_HOSTS:
        raise ToolError(f"refusing to talk to {host}: the daemon must be on loopback")
    return url.rstrip("/")


def call(method: str, path: str, query: dict[str, Any] | None = None, body: Any = None) -> Any:
    url = daemon_url() + path
    if query:
        url += "?" + urllib.parse.urlencode({k: v for k, v in query.items() if v not in (None, "")})
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode())
    except urllib.error.HTTPError as error:
        try:
            message = json.loads(error.read().decode())["error"]["message"]
        except (ValueError, KeyError):
            message = f"daemon returned status {error.code}"
        raise ToolError(message) from None
    except urllib.error.URLError as error:
        raise ToolError(f"cannot reach the daemon at {daemon_url()}: {error.reason}") from None


def require_text(name: str, value: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ToolError(f"{name} must be a non-empty string")
    return value


mcp = MCPServer("veylqora")


@mcp.tool()
def get_project_status(root: str) -> dict:
    """Index and memory counts for a project root."""
    return call("GET", "/api/v1/status", {"root": require_text("root", root)})


@mcp.tool()
def search_code(
    root: str,
    query: str,
    k: int = 10,
    mode: str = "hybrid",
    kind: str = "",
    path_prefix: str = "",
) -> dict:
    """Ranked code definitions for a free-text query. Each hit names its file and line."""
    if not 1 <= k <= 100:
        raise ToolError("k must be between 1 and 100")
    return call(
        "GET",
        "/api/v1/retrieve",
        {"root": require_text("root", root), "query": require_text("query", query), "k": k,
         "mode": mode, "kind": kind, "path_prefix": path_prefix},
    )


@mcp.tool()
def inspect_symbol(root: str, name: str) -> dict:
    """Where a named definition is, and what it depends on and what depends on it."""
    name = require_text("name", name)
    found = call("GET", "/api/v1/search", {"root": require_text("root", root), "name": name})
    graph = call("GET", "/api/v1/graph", {"root": root, "name": name})
    return {"definitions": found["hits"], "dependents": graph["dependents"],
            "dependencies": graph["dependencies"]}


@mcp.tool()
def find_dependencies(root: str, name: str) -> dict:
    """Calls, imports and inheritance around a name, each with its resolution level."""
    return call("GET", "/api/v1/graph", {"root": require_text("root", root), "name": require_text("name", name)})


@mcp.tool()
def search_memories(root: str, text: str = "", state: str = "") -> dict:
    """Stored memories whose content contains the text, optionally in one state."""
    memories = call("GET", "/api/v1/memories", {"root": require_text("root", root), "state": state})
    needle = text.lower()
    matches = [m for m in memories if not needle or needle in m["content"].lower()]
    return {"memories": matches}


@mcp.tool()
def record_memory(
    root: str,
    type: str,
    content: str,
    author: str,
    evidence: list[dict] | None = None,
    scope: str = "",
) -> dict:
    """Store a memory. Evidence entries are {"rel_path": ..., "symbol_fqn": ...} and must name indexed symbols."""
    body = {"root": require_text("root", root), "type": require_text("type", type),
            "content": require_text("content", content), "author": require_text("author", author),
            "evidence": evidence or [], "scope": scope}
    return call("POST", "/api/v1/memories", body=body)


@mcp.tool()
def inspect_evidence(root: str, memory_id: str) -> dict:
    """A memory with its evidence, revision history and state transitions."""
    return call("GET", "/api/v1/memories/" + require_text("memory_id", memory_id), {"root": root})


@mcp.tool()
def check_memory_validity(root: str, memory_id: str) -> dict:
    """Whether a memory still matches the code it cites. Flagged memories carry a warning."""
    memory = call("GET", "/api/v1/memories/" + require_text("memory_id", memory_id), {"root": root})
    flagged = memory["state"] in ("suspected_stale", "evidence_missing")
    warning = None
    if flagged:
        warning = "the code this memory cites changed or disappeared; review before relying on it"
    return {"id": memory["id"], "state": memory["state"], "valid": memory["state"] == "active",
            "warning": warning, "evidence": memory["evidence"]}


@mcp.tool()
def get_task_context(root: str, task: str, budget_tokens: int = 2000) -> dict:
    """A bounded, evidence-linked context package for a task (ADR-0016).

    Sections fill in order: warnings, then memories, then code. Each item is added
    whole or not at all, and anything left out is counted under `truncated`.
    Every memory carries its evidence, and no definition or memory appears twice.
    """
    task = require_text("task", task)
    if not 100 <= budget_tokens <= 100000:
        raise ToolError("budget_tokens must be between 100 and 100000")
    limit = budget_tokens * CHARS_PER_TOKEN
    used = 0
    sections: dict[str, list] = {"warnings": [], "memories": [], "code": []}
    truncated = {"warnings": 0, "memories": 0, "code": 0}
    seen_memories: set[str] = set()
    seen_code: set[str] = set()

    def take(section: str, entry: dict) -> bool:
        nonlocal used
        cost = len(json.dumps(entry))
        if used + cost > limit:
            truncated[section] += 1
            return False
        sections[section].append(entry)
        used += cost
        return True

    needle = task.lower().split()
    memories = call("GET", "/api/v1/memories", {"root": root})
    matching = [m for m in memories if any(w in m["content"].lower() for w in needle)]

    flagged = [m for m in matching if m["state"] in ("suspected_stale", "evidence_missing")]
    active = [m for m in matching if m["state"] == "active"]

    for m in flagged:
        if m["id"] in seen_memories:
            continue
        seen_memories.add(m["id"])
        detail = call("GET", "/api/v1/memories/" + m["id"], {"root": root})
        take("warnings", {"id": m["id"], "state": m["state"], "content": m["content"],
                          "warning": "the code this memory cites changed or disappeared; review it",
                          "evidence": detail["evidence"]})

    for m in active:
        if m["id"] in seen_memories:
            continue
        seen_memories.add(m["id"])
        detail = call("GET", "/api/v1/memories/" + m["id"], {"root": root})
        take("memories", {"id": m["id"], "content": m["content"], "evidence": detail["evidence"]})

    for h in call("GET", "/api/v1/retrieve", {"root": root, "query": task, "k": 50})["hits"]:
        if h["fqn"] in seen_code:
            continue
        seen_code.add(h["fqn"])
        take("code", h)

    return {"task": task, "budget_tokens": budget_tokens, "approx_tokens_used": used // CHARS_PER_TOKEN,
            "sections": sections, "truncated": truncated}


if __name__ == "__main__":
    mcp.run()
