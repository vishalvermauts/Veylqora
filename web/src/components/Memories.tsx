import { useState } from "react";
import { getJson } from "../api";
import { ResourceView, useResource } from "../useResource";
import type { MemoryDetail, MemorySummary } from "../types";

const STATES = ["", "active", "suspected_stale", "superseded", "invalidated", "evidence_missing", "archived"];

// Memories are listed with their state; selecting one shows its evidence,
// revision history and state transitions. Nothing here writes.
export function Memories({ root }: { root: string | null }) {
  const [state, setState] = useState("");
  const [selected, setSelected] = useState<string | null>(null);
  const listKey = root !== null ? `${root}\u0000${state}` : null;
  const list = useResource(listKey, (k) => {
    const [r, s] = k.split("\u0000");
    return getJson<MemorySummary[]>("/api/v1/memories", s ? { root: r, state: s } : { root: r });
  });
  const detailKey = root !== null && selected ? `${root}\u0000${selected}` : null;
  const detail = useResource(detailKey, (k) => {
    const [r, id] = k.split("\u0000");
    return getJson<MemoryDetail>(`/api/v1/memories/${encodeURIComponent(id)}`, { root: r });
  });

  return (
    <section aria-labelledby="memories-heading">
      <h2 id="memories-heading">Memories</h2>
      <label htmlFor="state-filter">State</label>{" "}
      <select id="state-filter" value={state} onChange={(e) => setState(e.target.value)}>
        {STATES.map((s) => (
          <option key={s} value={s}>
            {s === "" ? "all" : s}
          </option>
        ))}
      </select>
      <ResourceView resource={list} empty={(d) => d.length === 0}>
        {(memories) => (
          <ul aria-label="memories">
            {memories.map((m) => (
              <li key={m.id}>
                <button type="button" aria-pressed={selected === m.id} onClick={() => setSelected(m.id)}>
                  {m.content}
                </button>{" "}
                <span>
                  {m.state}, revision {m.revision}, by {m.author}
                </span>
              </li>
            ))}
          </ul>
        )}
      </ResourceView>
      {selected !== null && (
        <article aria-label="memory detail">
          <ResourceView resource={detail}>
            {(m) => (
              <>
                <h3>{m.content}</h3>
                <p>
                  {m.type}, {m.state}, scope {m.scope}, revision {m.revision}
                </p>
                <h4>Evidence</h4>
                {m.evidence.length === 0 ? (
                  <p>No evidence attached.</p>
                ) : (
                  <ul>
                    {m.evidence.map((e, i) => (
                      <li key={i}>
                        <code>{e.symbol}</code> in {e.path}
                      </li>
                    ))}
                  </ul>
                )}
                <h4>Revisions</h4>
                <ol>
                  {m.history.map((h) => (
                    <li key={h.revision}>
                      r{h.revision} — {h.state}, {h.actor}: {h.reason}
                    </li>
                  ))}
                </ol>
                <h4>State changes</h4>
                <ul>
                  {m.transitions.map((t, i) => (
                    <li key={i}>
                      {t.from} → {t.to} by {t.actor}: {t.reason}
                    </li>
                  ))}
                </ul>
              </>
            )}
          </ResourceView>
        </article>
      )}
    </section>
  );
}
