# ADR-0007 — MCP server is a Python process, pinned to mcp 2.3.0

- **Status:** accepted (spike S4)
- **Date:** 2026-10-20
- **Note:** numbered 0007 because ADR-0005 is taken by CI (cloud sandbox).

## Context

The product must expose tools to coding agents over MCP. The C++ core has no
maintained MCP implementation, and ADR-0002 and plan §3.1 rule out writing one
by hand. The question for S4 was whether a maintained SDK can carry a real
request/response round trip against a stub, with a version we can pin.

## Evidence (spike S4)

Stub server (`spikes/s4_mcp/server.py`, one tool) and client
(`spikes/s4_mcp/client.py`, the SDK's stdio client) ran in the cloud build
sandbox. Raw output: `docs/research/s4-mcp-handshake.txt`.

| Check | Result |
|---|---|
| Protocol negotiation | pass — `2025-11-25` |
| Tool discovery | pass — `search_code` listed |
| Valid call returns data | pass — structured result received |
| Missing required argument is rejected | pass — `is_error: true` |
| Unknown tool is rejected | pass — `is_error: true` |

Two findings that change the plan:

1. **Major-version break.** `mcp` 2.x renamed `FastMCP` to `MCPServer`
   (`mcp.server.mcpserver`) and changed result fields to snake_case
   (`protocol_version`, `structured_content`, `is_error`). Code written against
   the v1 documentation fails on import. An unpinned dependency would break
   silently on the next install.
2. **Latest release is v2.3.0.** The version pinned in
   `spikes/s4_mcp/requirements.txt` is `mcp==2.3.0`.

## Decision

- The MCP server is a Python process using `mcp==2.3.0`, exactly pinned.
  It talks to the C++ core over the local HTTP/JSON API (ADR-0004).
- Upgrading the SDK is a deliberate change that re-runs S4. It is never a
  side effect of installing.

## What S4 does not prove

- The client here is the SDK's own. Gate 2.5 requires at least one supported
  coding agent (for example an IDE coding agent) to complete an end-to-end workflow.
  That is Module 2.5 work, not Phase 0.
- Only stdio transport was tested. Remote transport and authentication are
  separate work and are not covered by this decision.
- The tool returns a fixed stub result. Real data flow starts in Module 2.5.

## Consequences

- The server needs a Python environment; `spikes/s4_mcp/requirements.txt` pins it.
- Protocol revision `2025-11-25` is what this decision was verified against.
