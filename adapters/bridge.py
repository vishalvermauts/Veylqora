#!/usr/bin/env python3
"""Outbound bridge (M3.5, contract veylqora.outbound.v1).

Reads memories from the local daemon, keeps only what the contract allows to
leave the machine, validates the batch against the schema, and pushes it to a
receiver over HTTP. Nothing here is specific to one product: any receiver that
accepts the contract can use it.

Rules, enforced in code and tested:
- Only shared memories, and approved memories with an approver, are sent.
- Private memories, memory content and code never leave the daemon.
- A non-local receiver must be reached over HTTPS. Plain HTTP is allowed only to
  a loopback address.
- Every push carries a bearer token from VEYLQORA_BRIDGE_TOKEN. The token is
  read from the environment, never from the command line or the repository.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

import jsonschema

CONTRACT = "veylqora.outbound.v1"
SCHEMA_PATH = pathlib.Path(__file__).resolve().parent / "contracts" / "outbound-v1.schema.json"
LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}
TOKEN_ENV = "VEYLQORA_BRIDGE_TOKEN"


class BridgeError(Exception):
    """A refusal by the bridge: a rule it enforces, not a failure of the receiver."""


def load_schema() -> dict[str, Any]:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def is_shareable(memory: dict[str, Any]) -> bool:
    """Shared memories are shareable. Approved memories are shareable only once an
    approver has signed them off (ADR-0017)."""
    if memory.get("scope") == "shared":
        return True
    return memory.get("scope") == "approved" and bool(memory.get("approved_by"))


def build_batch(memories: list[dict[str, Any]], project: str) -> dict[str, Any]:
    """Snapshot events for the memories that may leave. Content is not included."""
    events = [
        {
            "type": "memory.snapshot",
            "id": m["id"],
            "revision": int(m["revision"]),
            "state": m["state"],
            "scope": m["scope"],
            "at": m["updated_at"],
        }
        for m in memories
        if is_shareable(m)
    ]
    return {"contract": CONTRACT, "project": project, "events": events}


def validate(batch: dict[str, Any]) -> None:
    """Raises jsonschema.ValidationError if the batch does not match the contract."""
    jsonschema.validate(instance=batch, schema=load_schema())


def check_endpoint(endpoint: str) -> None:
    parts = urllib.parse.urlparse(endpoint)
    host = parts.hostname or ""
    if parts.scheme == "https":
        return
    if parts.scheme == "http" and host in LOOPBACK_HOSTS:
        return
    raise BridgeError(
        f"refusing {endpoint}: a remote receiver must use https; plain http is for loopback only"
    )


def push(endpoint: str, batch: dict[str, Any], token: str, timeout: float = 10.0) -> int:
    check_endpoint(endpoint)
    if not token:
        raise BridgeError(f"no token: set {TOKEN_ENV}")
    validate(batch)
    request = urllib.request.Request(
        endpoint,
        data=json.dumps(batch).encode("utf-8"),
        method="POST",
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status
    except urllib.error.HTTPError as error:
        raise BridgeError(f"receiver refused the batch: HTTP {error.code}") from None


def fetch_memories(daemon: str, root: str, viewer: str) -> list[dict[str, Any]]:
    query = urllib.parse.urlencode({"root": root, "viewer": viewer})
    with urllib.request.urlopen(f"{daemon}/api/v1/memories?{query}", timeout=10) as response:
        return json.loads(response.read().decode("utf-8"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--daemon", default="http://127.0.0.1:8765")
    parser.add_argument("--root", required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--viewer", default="bridge")
    args = parser.parse_args(argv)
    try:
        memories = fetch_memories(args.daemon, args.root, args.viewer)
        batch = build_batch(memories, args.project)
        status = push(args.endpoint, batch, os.environ.get(TOKEN_ENV, ""))
    except BridgeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"sent {len(batch['events'])} of {len(memories)} memories; receiver answered {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
