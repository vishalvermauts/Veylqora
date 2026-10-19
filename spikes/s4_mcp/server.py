"""Spike S4: stub MCP server with one tool and deterministic output.

No index, no storage: the spike only proves protocol negotiation, tool
discovery, schema validation and one round trip. It is throwaway.
"""

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("veylqora-stub")


@mcp.tool()
def search_code(query: str, limit: int = 5) -> dict:
    """Search indexed code for a symbol (stub: returns one fixed hit)."""
    return {
        "query": query,
        "limit": limit,
        "hits": [{"path": "src/core/identity.cpp", "symbol": "symbol_id", "score": 1.0}],
        "stub": True,
    }


if __name__ == "__main__":
    mcp.run()  # stdio transport
