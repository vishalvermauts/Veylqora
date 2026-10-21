import { render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, describe, expect, it, vi } from "vitest";
import { App } from "../App";

// Component tests for the retrieval inspector and the memory timeline. The
// daemon's responses are mocked; the tests check that the screens show what
// the responses say, in the order they say it.

function respond(body: unknown, status = 200) {
  return Promise.resolve(new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } }));
}

afterEach(() => vi.restoreAllMocks());

describe("retrieval inspector", () => {
  it("shows each mode's ranking with the score the server returned", async () => {
    const hits = (score: number) => ({
      query: "tokens",
      hits: [{ fqn: "auth.tokens.verify_token", path: "auth/tokens.py", kind: "function", line: 5, score }],
    });
    const byMode: Record<string, unknown> = { hybrid: hits(0.0312), lexical: hits(1.5), vector: hits(0.25) };
    const fetchMock = vi.fn((url: string) => respond(byMode[new URL(url, "http://x").searchParams.get("mode") ?? "hybrid"]));
    vi.stubGlobal("fetch", fetchMock);

    render(<App />);
    await userEvent.type(screen.getByLabelText("Project root"), "/r");
    await userEvent.click(screen.getByRole("button", { name: "Open" }));
    await userEvent.click(screen.getByRole("tab", { name: "Retrieval" }));
    await userEvent.type(screen.getByLabelText("Query"), "tokens");
    await userEvent.click(screen.getByRole("button", { name: "Inspect" }));

    const lexical = await screen.findByRole("region", { name: "lexical ranking" });
    expect(within(lexical).getByText(/score 1\.5000/)).toBeTruthy();
    expect(within(screen.getByRole("region", { name: "hybrid ranking" })).getByText(/score 0\.0312/)).toBeTruthy();
    expect(within(screen.getByRole("region", { name: "vector ranking" })).getByText(/score 0\.2500/)).toBeTruthy();
    // One request per mode, each with the query and the same root.
    const modes = fetchMock.mock.calls
      .map((c) => new URL(String(c[0]), "http://x"))
      .filter((u) => u.pathname === "/api/v1/retrieve")
      .map((u) => u.searchParams.get("mode"))
      .sort();
    expect(modes).toEqual(["hybrid", "lexical", "vector"]);
  });
});

describe("memory timeline", () => {
  const detail = {
    id: "m2",
    type: "decision",
    content: "second claim",
    state: "superseded",
    scope: "shared",
    author: "alice",
    revision: 2,
    superseded_by: "m3",
    created_at: "2026-01-02T00:00:00Z",
    updated_at: "2026-01-03T00:00:00Z",
    evidence: [],
    history: [
      { revision: 2, content: "second claim", state: "active", actor: "alice", reason: "edited", at: "2026-01-03T00:00:00Z" },
    ],
    transitions: [
      { from: "active", to: "superseded", actor: "bob", reason: "replaced", at: "2026-01-04T00:00:00Z" },
    ],
  };
  const list = [{ ...detail }, { ...detail, id: "m3", content: "newer claim", state: "active", superseded_by: undefined }];

  it("orders revisions and state changes by time and links to the superseding memory", async () => {
    const fetchMock = vi.fn((url: string) => {
      const path = new URL(url, "http://x").pathname;
      if (path === "/api/v1/memories/m2") return respond(detail);
      if (path === "/api/v1/memories/m3") {
        return respond({ ...detail, id: "m3", content: "newer claim", state: "active", superseded_by: undefined, history: [], transitions: [] });
      }
      return respond(list);
    });
    vi.stubGlobal("fetch", fetchMock);

    render(<App />);
    await userEvent.type(screen.getByLabelText("Project root"), "/r");
    await userEvent.click(screen.getByRole("button", { name: "Open" }));
    await userEvent.click(screen.getByRole("tab", { name: "Memories" }));
    await userEvent.click(await screen.findByRole("button", { name: "second claim" }));

    const timeline = await screen.findByRole("list", { name: "timeline" });
    const items = within(timeline).getAllByRole("listitem").map((li) => li.textContent ?? "");
    expect(items[0]).toContain("2026-01-03");
    expect(items[1]).toContain("2026-01-04");

    await userEvent.click(screen.getByRole("button", { name: "m3" }));
    await waitFor(() => expect(screen.getByRole("heading", { name: "newer claim" })).toBeTruthy());
  });
});
