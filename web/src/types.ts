// Shapes of the daemon's JSON responses (src/api/service.cpp).

export interface Status {
  project_root: string;
  indexed: boolean;
  project?: string;
  files: number;
  symbols: number;
  memories: number;
}

export interface SearchHit {
  path: string;
  kind: string;
  fqn: string;
  line: number;
}

export interface Dependent {
  kind: string;
  from: string;
  path: string;
  line: number;
  resolution: "resolved" | "heuristic" | "unresolved";
}

export interface Dependency {
  kind: string;
  to: string | null;
  hint: string;
  resolution: "resolved" | "heuristic" | "unresolved";
  line: number;
}

export interface GraphResult {
  name: string;
  dependents: Dependent[];
  dependencies: Dependency[];
}

export interface MemorySummary {
  id: string;
  type: string;
  content: string;
  state: string;
  scope: string;
  author: string;
  revision: number;
  superseded_by?: string;
  created_at: string;
  updated_at: string;
}

export interface MemoryDetail extends MemorySummary {
  evidence: { path: string; symbol: string; fingerprint: string }[];
  history: { revision: number; content: string; state: string; actor: string; reason: string; at: string }[];
  transitions: { from: string; to: string; actor: string; reason: string; at: string }[];
}
