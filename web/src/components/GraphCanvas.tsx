import { useEffect, useRef } from "react";
import cytoscape, { type ElementDefinition } from "cytoscape";
import type { GraphResult } from "../types";

// The symbol in the middle, what depends on it on one side, and what it depends
// on on the other. Every node and edge comes from the daemon's graph response.
// Edge style shows the resolution level: solid for resolved, dashed for
// heuristic, dotted for unresolved (ADR-0010). The text lists next to this
// canvas carry the same information for screen readers.
export function graphElements(graph: GraphResult): ElementDefinition[] {
  const nodes = new Map<string, ElementDefinition>();
  const edges: ElementDefinition[] = [];
  nodes.set(graph.name, { data: { id: graph.name, label: graph.name, role: "center" } });

  graph.dependents.forEach((e, i) => {
    if (!nodes.has(e.from)) {
      nodes.set(e.from, { data: { id: e.from, label: e.from, role: "dependent" } });
    }
    edges.push({
      data: { id: `in-${i}`, source: e.from, target: graph.name, label: e.kind, resolution: e.resolution },
    });
  });

  graph.dependencies.forEach((e, i) => {
    // An unresolved dependency has no target name, so it gets its own node, keyed by its text.
    const id = e.to ?? `unknown:${e.hint}`;
    if (!nodes.has(id)) {
      nodes.set(id, { data: { id, label: e.to ?? e.hint, role: e.to ? "dependency" : "unknown" } });
    }
    edges.push({
      data: { id: `out-${i}`, source: graph.name, target: id, label: e.kind, resolution: e.resolution },
    });
  });

  return [...nodes.values(), ...edges];
}

export function GraphCanvas({ graph }: { graph: GraphResult }) {
  const container = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (container.current === null) return;
    const cy = cytoscape({
      container: container.current,
      elements: graphElements(graph),
      layout: { name: "breadthfirst", directed: true, spacingFactor: 1.2 },
      style: [
        {
          selector: "node",
          style: {
            label: "data(label)",
            "font-size": 11,
            "text-wrap": "wrap",
            "text-max-width": "140px",
            "background-color": "#4a6fa5",
            color: "#111",
          },
        },
        { selector: 'node[role = "center"]', style: { "background-color": "#1f3a5f", color: "#fff" } },
        { selector: 'node[role = "unknown"]', style: { "background-color": "#c9c9c9", shape: "round-rectangle" } },
        {
          selector: "edge",
          style: {
            "curve-style": "bezier",
            "target-arrow-shape": "triangle",
            "line-color": "#666",
            "target-arrow-color": "#666",
            width: 1.5,
            label: "data(label)",
            "font-size": 9,
          },
        },
        { selector: 'edge[resolution = "heuristic"]', style: { "line-style": "dashed" } },
        { selector: 'edge[resolution = "unresolved"]', style: { "line-style": "dotted" } },
      ],
    });
    return () => cy.destroy();
  }, [graph]);

  return (
    <div
      ref={container}
      className="graph-canvas"
      role="img"
      aria-label={`Graph of ${graph.name}: ${graph.dependents.length} dependents, ${graph.dependencies.length} dependencies`}
      style={{ height: 360, border: "1px solid #ccc" }}
    />
  );
}
