// Thin client for the daemon's read API. Same origin, so no CORS. Every failure
// becomes an ApiError with the server's stable code, never a raw fetch error.

export class ApiError extends Error {
  readonly status: number;
  readonly code: string;

  constructor(message: string, status: number, code: string) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.code = code;
  }
}

export async function getJson<T>(path: string, params: Record<string, string> = {}): Promise<T> {
  const query = new URLSearchParams(params).toString();
  let res: Response;
  try {
    res = await fetch(query ? `${path}?${query}` : path);
  } catch {
    throw new ApiError("Cannot reach the Veylqora server. Is `veylqora serve` running?", 0, "network");
  }
  const body: unknown = await res.json().catch(() => null);
  if (!res.ok) {
    const err = (body as { error?: { message?: string; code?: string } } | null)?.error;
    throw new ApiError(err?.message ?? `request failed (${res.status})`, res.status, err?.code ?? "http");
  }
  return body as T;
}
