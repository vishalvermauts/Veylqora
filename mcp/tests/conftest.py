"""Shared fixtures for the MCP end-to-end tests: one real daemon per module."""

import json
import os
import subprocess
import tempfile
import urllib.request

import pytest

BIN = os.environ.get("VEYLQORA_BIN")
FIXTURES = os.environ.get("VEYLQORA_FIXTURES")


@pytest.fixture(scope="module")
def daemon():
    work = tempfile.mkdtemp()
    proc = subprocess.Popen([BIN, "serve", "--db", os.path.join(work, "v.db"), "--port", "0"],
                            stdout=subprocess.PIPE, text=True)
    line = proc.stdout.readline()
    assert line.startswith("listening on http://127.0.0.1:"), line
    url = line.split()[2].rstrip()
    assert post_json(url, "/api/v1/index", {"root": FIXTURES})["indexed"] == 10
    yield url
    proc.terminate()
    proc.wait(timeout=10)


def post_json(url, path, body):
    req = urllib.request.Request(url + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read())


def get_json(url, path):
    with urllib.request.urlopen(url + path) as r:
        return json.loads(r.read())
