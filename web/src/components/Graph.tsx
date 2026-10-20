import { useState } from "react";
import { getJson } from "../api";
import { ResourceView, useResource } from "../useResource";
import type { GraphResult } from "../types";

// A list view of the code graph: what depends on a symbol, and what it depends
// on, each with its resolution level so guesses are visibly guesses (ADR-0010).
export function Graph({ root }: { root: string | null }) {
  const [draft, setDraft] = useState("");
  const [name, setName] = useState<string | null>(null);
  const key = root !== null && name ? `${root}\u0000${name}` : null;
  const resource = useResource(key, (k) => {
    const [r, n] = k.split("\u0000");
    return getJson<GraphResult>("/api/v1/graph", { root: r, name: n });
  });

  return (
    <section aria-labelledby="graph-heading">
      <h2 id="graph-heading">Code graph</h2>
      <form
        onSubmit={(e) => {
          e.preventDefault();
          setName(draft.trim() || null);
        }}
      >
        <label htmlFor="graph-name">Symbol name</label>{" "}
        <input id="graph-name" value={draft} onChange={(e) => setDraft(e.target.value)} />{" "}
        <button type="submit" disabled={root === null}>
          Show
        </button>
      </form>
      <ResourceView resource={resource} empty={(d) => d.dependents.length + d.dependencies.length === 0}>
        {(d) => (
          <div className="graph-columns">
            <section aria-label="dependents">
              <h3>Depends on {d.name}</h3>
              <ul>
                {d.dependents.map((e, i) => (
                  <li key={i}>
                    <code>{e.from}</code> {e.kind} [{e.resolution}] {e.path}:{e.line}
                  </li>
                ))}
              </ul>
            </section>
            <section aria-label="dependencies">
              <h3>{d.name} depends on</h3>
              <ul>
                {d.dependencies.map((e, i) => (
                  <li key={i}>
                    {e.to ? <code>{e.to}</code> : <span>unknown: {e.hint}</span>} {e.kind} [{e.resolution}]
                  </li>
                ))}
              </ul>
            </section>
          </div>
        )}
      </ResourceView>
    </section>
  );
}
