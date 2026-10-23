"""Model Context Protocol server for Veylqora (M2.5, ADR-0007).

A thin process: it turns MCP tool calls into requests to the local Veylqora
daemon and returns the daemon's answers unchanged. It stores nothing itself.
The daemon must be on a loopback address; any other address is refused.
"""

from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError as SdkToolError

LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}

# A rough token estimate for context budgeting: about four characters per token.
CHARS_PER_TOKEN = 4

STOPWORDS = {
    "a", "about", "above", "after", "again", "all", "also", "always", "am", "an",
    "and", "any", "are", "as", "at", "be", "because", "been", "before", "being",
    "below", "between", "both", "but", "by", "can", "cannot", "could", "def", "did",
    "do", "does", "doing", "done", "down", "during", "each", "else", "even", "every",
    "false", "for", "from", "further", "get", "gets", "got", "had", "has", "have",
    "having", "he", "her", "here", "hers", "him", "his", "how", "however", "if",
    "in", "into", "is", "it", "its", "itself", "just", "let", "like", "make", "many",
    "may", "me", "might", "more", "most", "much", "must", "my", "need", "new", "no",
    "none", "nor", "not", "now", "null", "of", "off", "on", "once", "one", "only",
    "or", "other", "our", "out", "over", "own", "pass", "return", "returns", "same",
    "see", "self", "set", "she", "should", "since", "so", "some", "such", "take",
    "than", "that", "the", "their", "them", "then", "there", "these", "they", "this",
    "those", "through", "to", "too", "true", "under", "until", "up", "use", "used",
    "using", "var", "very", "via", "want", "was", "way", "we", "well", "were",
    "what", "when", "where", "which", "while", "who", "whom", "why", "will", "with",
    "without", "work", "works", "would", "yes", "yet", "you", "your",
}


def tokenize_text(text: str) -> list[str]:
    """Extract normalized non-stopword tokens and compound identifiers."""
    out: list[str] = []
    seen: set[str] = set()
    for word in re.findall(r"[A-Za-z0-9_]+", text):
        low = word.lower().strip("_")
        if len(low) >= 2 and low not in STOPWORDS and low not in seen:
            seen.add(low)
            out.append(low)
        spaced = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", word)
        spaced = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", spaced)
        for part in re.split(r"[^A-Za-z0-9]+", spaced):
            p = part.lower()
            if len(p) >= 2 and p not in STOPWORDS and p not in seen:
                seen.add(p)
                out.append(p)
    return out


def memory_score(query_tokens: list[str], content: str, evidence: list[dict] | None = None) -> int:
    """Count meaningful token overlaps against memory content and cited evidence."""
    if not query_tokens:
        return 1
    doc_tokens = set(tokenize_text(content))
    for ev in evidence or []:
        doc_tokens.update(tokenize_text(ev.get("symbol", "")))
        doc_tokens.update(tokenize_text(ev.get("path", "")))
    score = 0
    for qt in query_tokens:
        if qt in doc_tokens:
            score += 2
        elif len(qt) >= 4 and any(dt.startswith(qt) or qt.startswith(dt) for dt in doc_tokens if len(dt) >= 4):
            score += 1
    return score


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


def resolve_root(root: str) -> str:
    """The project root: the argument if given, else VEYLQORA_DEFAULT_ROOT. Agents often
    cannot know the exact path, so the default lets them omit it."""
    value = root or os.environ.get("VEYLQORA_DEFAULT_ROOT", "")
    if not value:
        raise ToolError("root is required: pass the project root, or set VEYLQORA_DEFAULT_ROOT")
    return value


def resolve_agent(agent: str) -> str:
    """The agent identity for memory tools: the argument if given, else VEYLQORA_AGENT, else 'agent'."""
    return agent or os.environ.get("VEYLQORA_AGENT", "agent")


def require_text(name: str, value: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ToolError(f"{name} must be a non-empty string")
    return value


mcp = MCPServer("veylqora")


@mcp.tool()
def get_project_status(root: str = "") -> dict:
    """Index and memory counts for a project root."""
    return call("GET", "/api/v1/status", {"root": resolve_root(root)})


@mcp.tool()
def search_code(
    root: str = "",
    query: str = "",
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
        {"root": resolve_root(root), "query": require_text("query", query), "k": k,
         "mode": mode, "kind": kind, "path_prefix": path_prefix},
    )


@mcp.tool()
def inspect_symbol(root: str = "", name: str = "") -> dict:
    """Where a named definition is, and what it depends on and what depends on it."""
    name = require_text("name", name)
    found = call("GET", "/api/v1/search", {"root": resolve_root(root), "name": name})
    graph = call("GET", "/api/v1/graph", {"root": resolve_root(root), "name": name})
    return {"definitions": found["hits"], "dependents": graph["dependents"],
            "dependencies": graph["dependencies"]}


@mcp.tool()
def find_dependencies(root: str = "", name: str = "") -> dict:
    """Calls, imports and inheritance around a name, each with its resolution level."""
    return call("GET", "/api/v1/graph", {"root": resolve_root(root), "name": require_text("name", name)})


@mcp.tool()
def search_memories(root: str = "", agent: str = "", text: str = "", state: str = "") -> dict:
    """Memories this agent may see whose content contains the text, optionally in one state."""
    resolved_root = resolve_root(root)
    viewer = resolve_agent(agent)
    memories = call("GET", "/api/v1/memories",
                    {"root": resolved_root, "state": state, "viewer": viewer})
    if not text.strip():
        return {"memories": memories}
    qtokens = tokenize_text(text)
    scored: list[tuple[int, dict]] = []
    for m in memories:
        s = memory_score(qtokens, m["content"])
        if s == 0:
            detail = call("GET", "/api/v1/memories/" + m["id"], {"root": resolved_root, "viewer": viewer})
            s = memory_score(qtokens, m["content"], detail.get("evidence", []))
        if s > 0:
            scored.append((s, m))
    scored.sort(key=lambda item: (-item[0], item[1]["id"]))
    return {"memories": [m for _, m in scored]}


@mcp.tool()
def record_memory(
    root: str = "",
    type: str = "",
    content: str = "",
    agent: str = "",
    evidence: list[dict] | None = None,
    scope: str = "",
) -> dict:
    """Store a memory. Evidence entries are {"rel_path": ..., "symbol_fqn": ...} and must name indexed symbols."""
    body = {"root": resolve_root(root), "type": require_text("type", type),
            "content": require_text("content", content), "author": resolve_agent(agent),
            "evidence": evidence or [], "scope": scope}
    return call("POST", "/api/v1/memories", body=body)


@mcp.tool()
def inspect_evidence(memory_id: str = "", root: str = "", agent: str = "") -> dict:
    """A memory this agent may see, with its evidence, history and transitions."""
    return call("GET", "/api/v1/memories/" + require_text("memory_id", memory_id),
                {"root": resolve_root(root), "viewer": resolve_agent(agent)})


@mcp.tool()
def check_memory_validity(memory_id: str = "", root: str = "", agent: str = "") -> dict:
    """Whether a memory this agent may see still matches the code it cites."""
    memory = call("GET", "/api/v1/memories/" + require_text("memory_id", memory_id),
                  {"root": resolve_root(root), "viewer": resolve_agent(agent)})
    flagged = memory["state"] in ("suspected_stale", "evidence_missing")
    warning = None
    if flagged:
        warning = "the code this memory cites changed or disappeared; review before relying on it"
    return {"id": memory["id"], "state": memory["state"], "valid": memory["state"] == "active",
            "warning": warning, "evidence": memory["evidence"]}


@mcp.tool()
def get_task_context(task: str = "", root: str = "", agent: str = "", budget_tokens: int = 2000) -> dict:
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

    resolved_root = resolve_root(root)
    viewer = resolve_agent(agent)
    qtokens = tokenize_text(task)
    memories = call("GET", "/api/v1/memories", {"root": resolved_root, "viewer": viewer})
    code_hits = call("GET", "/api/v1/retrieve", {"root": resolved_root, "query": task, "k": 50})["hits"]
    top_symbols = {h["fqn"] for h in code_hits[:10]}
    top_paths = {h["path"] for h in code_hits[:5]}

    scored_memories: list[tuple[int, dict, list[dict]]] = []
    for m in memories:
        if m["state"] not in ("active", "suspected_stale", "evidence_missing"):
            continue
        detail = call("GET", "/api/v1/memories/" + m["id"], {"root": resolved_root, "viewer": viewer})
        evidence = detail.get("evidence", [])
        s = memory_score(qtokens, m["content"], evidence)
        for ev in evidence:
            if ev.get("symbol") in top_symbols:
                s += 3
            elif ev.get("path") in top_paths:
                s += 1
        if s > 0:
            scored_memories.append((s, m, evidence))

    scored_memories.sort(key=lambda item: (-item[0], item[1]["id"]))

    for _, m, evidence in scored_memories:
        if m["state"] not in ("suspected_stale", "evidence_missing"):
            continue
        if m["id"] in seen_memories:
            continue
        seen_memories.add(m["id"])
        take("warnings", {"id": m["id"], "state": m["state"], "content": m["content"],
                          "warning": "the code this memory cites changed or disappeared; review it",
                          "evidence": evidence})

    for _, m, evidence in scored_memories:
        if m["state"] != "active":
            continue
        if m["id"] in seen_memories:
            continue
        seen_memories.add(m["id"])
        take("memories", {"id": m["id"], "content": m["content"], "evidence": evidence})

    for h in code_hits:
        if h["fqn"] in seen_code:
            continue
        seen_code.add(h["fqn"])
        take("code", h)

    return {"task": task, "budget_tokens": budget_tokens, "approx_tokens_used": used // CHARS_PER_TOKEN,
            "sections": sections, "truncated": truncated}


if __name__ == "__main__":
    mcp.run()
