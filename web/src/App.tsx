import { useState, type KeyboardEvent } from "react";
import { Graph } from "./components/Graph";
import { Memories } from "./components/Memories";
import { Overview } from "./components/Overview";
import { Retrieval } from "./components/Retrieval";
import { Search } from "./components/Search";

const VIEWS = [
  { id: "overview", label: "Overview", render: (root: string | null) => <Overview root={root} /> },
  { id: "search", label: "Search", render: (root: string | null) => <Search root={root} /> },
  { id: "retrieval", label: "Retrieval", render: (root: string | null) => <Retrieval root={root} /> },
  { id: "graph", label: "Graph", render: (root: string | null) => <Graph root={root} /> },
  { id: "memories", label: "Memories", render: (root: string | null) => <Memories root={root} /> },
] as const;

type ViewId = (typeof VIEWS)[number]["id"];

export function App() {
  const [draftRoot, setDraftRoot] = useState("");
  const [root, setRoot] = useState<string | null>(null);
  const [view, setView] = useState<ViewId>("overview");

  // Arrow keys move between tabs, as the WAI-ARIA tabs pattern expects.
  const onTabKey = (e: KeyboardEvent<HTMLButtonElement>) => {
    const index = VIEWS.findIndex((v) => v.id === view);
    if (e.key !== "ArrowRight" && e.key !== "ArrowLeft") return;
    const next = (index + (e.key === "ArrowRight" ? 1 : VIEWS.length - 1)) % VIEWS.length;
    setView(VIEWS[next].id);
  };

  const current = VIEWS.find((v) => v.id === view) ?? VIEWS[0];

  return (
    <div className="app">
      <header>
        <h1>Veylqora</h1>
        <form
          onSubmit={(e) => {
            e.preventDefault();
            setRoot(draftRoot.trim() || null);
          }}
        >
          <label htmlFor="project-root">Project root</label>{" "}
          <input
            id="project-root"
            value={draftRoot}
            placeholder="/path/to/repository"
            onChange={(e) => setDraftRoot(e.target.value)}
          />{" "}
          <button type="submit">Open</button>
        </form>
      </header>
      {root === null && <p>Enter a project root to begin. The project must be indexed.</p>}
      <nav aria-label="views">
        <div role="tablist">
          {VIEWS.map((v) => (
            <button
              key={v.id}
              role="tab"
              type="button"
              id={`tab-${v.id}`}
              aria-selected={view === v.id}
              aria-controls="view-panel"
              tabIndex={view === v.id ? 0 : -1}
              onClick={() => setView(v.id)}
              onKeyDown={onTabKey}
            >
              {v.label}
            </button>
          ))}
        </div>
      </nav>
      <main id="view-panel" role="tabpanel" aria-labelledby={`tab-${view}`}>
        {current.render(root)}
      </main>
    </div>
  );
}
