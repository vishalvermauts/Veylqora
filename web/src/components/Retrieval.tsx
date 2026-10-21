import { useState } from "react";
import { getJson } from "../api";
import { ResourceView, useResource } from "../useResource";
import type { RetrieveResponse } from "../types";

const MODES = ["hybrid", "lexical", "vector"] as const;

type Modes = Record<(typeof MODES)[number], RetrieveResponse>;

// Runs one query through all three retrieval modes and shows each ranking with
// the score the server returned. Every number comes from the response; none is
// computed here.
export function Retrieval({ root }: { root: string | null }) {
  const [draft, setDraft] = useState("");
  const [query, setQuery] = useState<string | null>(null);
  const key = root !== null && query ? `${root}\u0000${query}` : null;
  const resource = useResource(key, async (k) => {
    const [r, q] = k.split("\u0000");
    const results = await Promise.all(
      MODES.map((mode) => getJson<RetrieveResponse>("/api/v1/retrieve", { root: r, query: q, mode, k: "10" })),
    );
    return Object.fromEntries(MODES.map((mode, i) => [mode, results[i]])) as Modes;
  });

  return (
    <section aria-labelledby="retrieval-heading">
      <h2 id="retrieval-heading">Retrieval inspector</h2>
      <form
        onSubmit={(e) => {
          e.preventDefault();
          setQuery(draft.trim() || null);
        }}
      >
        <label htmlFor="retrieval-query">Query</label>{" "}
        <input id="retrieval-query" value={draft} onChange={(e) => setDraft(e.target.value)} />{" "}
        <button type="submit" disabled={root === null}>
          Inspect
        </button>
      </form>
      <ResourceView resource={resource}>
        {(modes) => (
          <div className="retrieval-columns">
            {MODES.map((mode) => (
              <section key={mode} aria-label={`${mode} ranking`}>
                <h3>{mode}</h3>
                {modes[mode].hits.length === 0 ? (
                  <p>No matches.</p>
                ) : (
                  <ol>
                    {modes[mode].hits.map((h) => (
                      <li key={`${h.path}:${h.line}:${h.fqn}`}>
                        <code>{h.fqn}</code> — score {h.score.toFixed(4)}, {h.path}:{h.line}
                      </li>
                    ))}
                  </ol>
                )}
              </section>
            ))}
          </div>
        )}
      </ResourceView>
    </section>
  );
}
