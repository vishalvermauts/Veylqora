import { describe, expect, it } from "vitest";
import { graphElements } from "../components/GraphCanvas";
import type { GraphResult } from "../types";

// The canvas must draw exactly what the daemon returned: one edge per dependent
// and per dependency, each carrying its resolution level, and unresolved names
// kept as their own nodes.
const graph: GraphResult = {
  name: "verify",
  dependents: [{ kind: "calls", from: "api.routes.handle", path: "api/routes.py", line: 4, resolution: "resolved" }],
  dependencies: [
    { kind: "calls", to: "auth.tokens.check", hint: "check", resolution: "heuristic", line: 9 },
    { kind: "calls", to: null, hint: "getattr(mod, name)", resolution: "unresolved", line: 12 },
  ],
};

describe("graphElements", () => {
  const elements = graphElements(graph);
  const nodes = elements.filter((e) => !e.data.source);
  const edges = elements.filter((e) => e.data.source);

  it("puts the symbol in the middle with its dependents and dependencies around it", () => {
    const center = nodes.find((n) => n.data.id === "verify");
    expect(center?.data.role).toBe("center");
    expect(nodes.find((n) => n.data.id === "api.routes.handle")?.data.role).toBe("dependent");
    expect(nodes.find((n) => n.data.id === "auth.tokens.check")?.data.role).toBe("dependency");
  });

  it("gives every relation one edge with its resolution level", () => {
    expect(edges).toHaveLength(3);
    const byResolution = edges.map((e) => e.data.resolution).sort();
    expect(byResolution).toEqual(["heuristic", "resolved", "unresolved"]);
    const incoming = edges.find((e) => e.data.resolution === "resolved");
    expect(incoming?.data.source).toBe("api.routes.handle");
    expect(incoming?.data.target).toBe("verify");
  });

  it("keeps an unresolved dependency as its own node, labelled with its text", () => {
    const unknown = nodes.find((n) => n.data.role === "unknown");
    expect(unknown?.data.label).toBe("getattr(mod, name)");
  });
});
