import { useState } from "react";
import { getJson } from "../api";
import { ResourceView, useResource } from "../useResource";
import type { SearchHit } from "../types";

interface SearchResponse {
  name: string;
  hits: SearchHit[];
}

// The query is submitted, not typed through, so each keystroke is not a request.
export function Search({ root }: { root: string | null }) {
  const [draft, setDraft] = useState("");
  const [query, setQuery] = useState<string | null>(null);
  const key = root !== null && query ? `${root}\u0000${query}` : null;
  const resource = useResource(key, (k) => {
    const [r, name] = k.split("\u0000");
    return getJson<SearchResponse>("/api/v1/search", { root: r, name });
  });

  return (
    <section aria-labelledby="search-heading">
      <h2 id="search-heading">Search symbols</h2>
      <form
        onSubmit={(e) => {
          e.preventDefault();
          setQuery(draft.trim() || null);
        }}
      >
        <label htmlFor="search-name">Symbol name</label>{" "}
        <input id="search-name" value={draft} onChange={(e) => setDraft(e.target.value)} />{" "}
        <button type="submit" disabled={root === null}>
          Search
        </button>
      </form>
      <ResourceView resource={resource} empty={(d) => d.hits.length === 0}>
        {(d) => (
          <ul aria-label="search results">
            {d.hits.map((h) => (
              <li key={`${h.path}:${h.line}:${h.fqn}`}>
                <code>{h.fqn}</code> — {h.kind}, {h.path}:{h.line}
              </li>
            ))}
          </ul>
        )}
      </ResourceView>
    </section>
  );
}
