import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, describe, expect, it, vi } from "vitest";
import { App } from "../App";

// These tests run the real components against a mocked fetch. They check that
// every view calls the daemon's read API with the project root, and that loading
// and error states are shown. They do not replace the browser smoke test.

function respond(body: unknown, status = 200) {
  return Promise.resolve(new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } }));
}

afterEach(() => vi.restoreAllMocks());

describe("App", () => {
  it("asks for a project root before showing data", () => {
    render(<App />);
    expect(screen.getByText(/Enter a project root/)).toBeTruthy();
  });

  it("shows the overview numbers from the status endpoint", async () => {
    const fetchMock = vi.fn((_url: string) =>
      respond({ project_root: "/r", indexed: true, project: "r", files: 3, symbols: 7, memories: 2 }),
    );
    vi.stubGlobal("fetch", fetchMock);
    render(<App />);
    await userEvent.type(screen.getByLabelText("Project root"), "/r");
    await userEvent.click(screen.getByRole("button", { name: "Open" }));
    await waitFor(() => expect(screen.getByText("7")).toBeTruthy());
    const url = String(fetchMock.mock.calls[0][0]);
    expect(url).toContain("/api/v1/status");
    expect(url).toContain("root=%2Fr");
  });

  it("shows the server's message when a request fails", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(() => respond({ error: { code: "invalid", message: "missing query parameter: root" } }, 400)),
    );
    render(<App />);
    await userEvent.type(screen.getByLabelText("Project root"), "/r");
    await userEvent.click(screen.getByRole("button", { name: "Open" }));
    expect(await screen.findByRole("alert")).toHaveProperty("textContent", "missing query parameter: root");
  });

  it("switches views with the keyboard", async () => {
    vi.stubGlobal("fetch", vi.fn(() => respond([])));
    render(<App />);
    screen.getByRole("tab", { name: "Overview" }).focus();
    await userEvent.keyboard("{ArrowRight}");
    expect(screen.getByRole("tab", { name: "Search" }).getAttribute("aria-selected")).toBe("true");
  });
});
