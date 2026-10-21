"""Contract tests for the outbound bridge (M3.5). The receiver is a fake HTTP server
on loopback that records what it is sent and checks the bearer token, so these
tests exercise the real HTTP path without any product's service."""

import http.server
import json
import pathlib
import sys
import threading

import jsonschema
import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import bridge  # noqa: E402

TOKEN = "test-token-not-a-secret"


def memory(id_, scope="shared", state="active", approved_by="", content="private words"):
    return {
        "id": id_,
        "type": "decision",
        "content": content,
        "state": state,
        "scope": scope,
        "author": "alice",
        "revision": 2,
        "approved_by": approved_by,
        "updated_at": "2026-10-20T10:00:00Z",
    }


class FakeReceiver:
    """Accepts a batch only with the right bearer token; keeps what it received."""

    def __init__(self):
        self.received = []
        self.tokens = []
        receiver = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length)
                auth = self.headers.get("Authorization", "")
                receiver.tokens.append(auth)
                if auth != f"Bearer {TOKEN}":
                    self.send_response(401)
                    self.end_headers()
                    return
                receiver.received.append(json.loads(body))
                self.send_response(200)
                self.end_headers()

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}/ingest"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def receiver():
    fake = FakeReceiver()
    yield fake
    fake.close()


def test_only_shareable_memories_are_in_the_batch():
    memories = [
        memory("m-shared", scope="shared"),
        memory("m-private", scope="private"),
        memory("m-approved", scope="approved", approved_by="carol"),
        memory("m-unapproved", scope="approved", approved_by=""),
    ]
    batch = bridge.build_batch(memories, "demo")
    ids = [e["id"] for e in batch["events"]]
    assert ids == ["m-shared", "m-approved"]


def test_memory_content_never_leaves_the_daemon():
    batch = bridge.build_batch([memory("m1", content="SECRET WORDS")], "demo")
    assert "SECRET WORDS" not in json.dumps(batch)
    assert set(batch["events"][0]) == {"type", "id", "revision", "state", "scope", "at"}


def test_batch_matches_the_contract_schema():
    batch = bridge.build_batch([memory("m1"), memory("m2", scope="approved", approved_by="c")], "demo")
    bridge.validate(batch)  # raises if not


def test_a_batch_outside_the_contract_is_refused():
    batch = bridge.build_batch([memory("m1")], "demo")
    batch["events"][0]["content"] = "smuggled"  # not in the contract
    with pytest.raises(jsonschema.ValidationError):
        bridge.validate(batch)


def test_a_plain_http_receiver_on_another_host_is_refused():
    batch = bridge.build_batch([memory("m1")], "demo")
    with pytest.raises(bridge.BridgeError, match="https"):
        bridge.push("http://example.com/ingest", batch, TOKEN)


def test_https_to_a_remote_host_is_allowed_by_the_rule():
    bridge.check_endpoint("https://receiver.example.com/ingest")  # no exception


def test_push_without_a_token_is_refused(receiver):
    batch = bridge.build_batch([memory("m1")], "demo")
    with pytest.raises(bridge.BridgeError, match="no token"):
        bridge.push(receiver.url, batch, "")
    assert receiver.received == []


def test_push_sends_the_batch_with_the_bearer_token(receiver):
    batch = bridge.build_batch([memory("m1"), memory("m-private", scope="private")], "demo")
    status = bridge.push(receiver.url, batch, TOKEN)
    assert status == 200
    assert receiver.tokens == [f"Bearer {TOKEN}"]
    assert [e["id"] for e in receiver.received[0]["events"]] == ["m1"]


def test_a_receiver_that_refuses_the_token_is_reported(receiver):
    batch = bridge.build_batch([memory("m1")], "demo")
    with pytest.raises(bridge.BridgeError, match="HTTP 401"):
        bridge.push(receiver.url, batch, "wrong-token")
