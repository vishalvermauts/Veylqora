import { useEffect, useState, type ReactNode } from "react";
import { ApiError } from "./api";

export type Resource<T> =
  | { status: "idle" }
  | { status: "loading" }
  | { status: "error"; error: ApiError }
  | { status: "ready"; data: T };

// Runs `load` whenever `key` changes and exposes its state. A stale response
// from an earlier key never overwrites the current one.
export function useResource<T>(key: string | null, load: (key: string) => Promise<T>): Resource<T> {
  const [state, setState] = useState<Resource<T>>({ status: "idle" });

  useEffect(() => {
    if (key === null) {
      setState({ status: "idle" });
      return;
    }
    let current = true;
    setState({ status: "loading" });
    load(key).then(
      (data) => current && setState({ status: "ready", data }),
      (error: unknown) => {
        if (!current) return;
        const err = error instanceof ApiError ? error : new ApiError(String(error), 0, "unknown");
        setState({ status: "error", error: err });
      },
    );
    return () => {
      current = false;
    };
    // `load` is stable per view; only the key drives a reload.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key]);

  return state;
}

// Renders the loading and error states once, so every view handles them alike.
export function ResourceView<T>({
  resource,
  empty,
  children,
}: {
  resource: Resource<T>;
  empty?: (data: T) => boolean;
  children: (data: T) => ReactNode;
}) {
  if (resource.status === "idle") return null;
  if (resource.status === "loading") return <p role="status">Loading…</p>;
  if (resource.status === "error") {
    return (
      <p role="alert" className="error">
        {resource.error.message}
      </p>
    );
  }
  if (empty?.(resource.data)) return <p>Nothing to show.</p>;
  return <>{children(resource.data)}</>;
}
