import { getJson } from "../api";
import { ResourceView, useResource } from "../useResource";
import type { Status } from "../types";

export function Overview({ root }: { root: string | null }) {
  const resource = useResource(root, (r) => getJson<Status>("/api/v1/status", { root: r }));
  return (
    <section aria-labelledby="overview-heading">
      <h2 id="overview-heading">Overview</h2>
      <ResourceView resource={resource}>
        {(s) => (
          <dl className="facts">
            <dt>Project root</dt>
            <dd>{s.project_root}</dd>
            <dt>Indexed</dt>
            <dd>{s.indexed ? "yes" : "no — run `veylqora index <root>` first"}</dd>
            <dt>Files</dt>
            <dd>{s.files}</dd>
            <dt>Symbols</dt>
            <dd>{s.symbols}</dd>
            <dt>Memories</dt>
            <dd>{s.memories}</dd>
          </dl>
        )}
      </ResourceView>
    </section>
  );
}
