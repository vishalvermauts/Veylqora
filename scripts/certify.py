#!/usr/bin/env python3
"""Certify a module at HEAD and write docs/certification/<module>/<sha>/manifest.json.

Usage: python scripts/certify.py <module>

Rules (ADR-0006):
  * the working tree must be clean and HEAD must be pushed, because the checks
    run against origin, and the manifest must name exactly what was tested;
  * every mandatory check runs; a check that cannot run is a failure, not a skip;
  * status is PASS only if every check exits 0, PARTIAL if some do, else FAIL;
  * module source files are recorded by git blob id, so a later change to any of
    them makes the manifest stale (verify_certs.py), and EOL conversion on
    Windows checkouts does not change the ids.
"""

import datetime
import fnmatch
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CERT_DIR = ROOT / "docs" / "certification"

# Source globs are matched against repo-relative paths, as git lists them.
MODULES = {
    "0.1": {
        "title": "Bootstrap build, identity model, CI",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*",
            "src/*", "tests/*", "scripts/ci.sh", "scripts/certify.py",
            "scripts/verify_certs.py",
        ],
        "mandatory": {
            # One sandbox session runs all four jobs; ci.sh exits with the
            # number of failed jobs, so 0 means every job passed.
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san"],
        },
    },
    "2.1": {
        "title": "Hybrid retrieval: lexical, hashed vector, graph, reciprocal rank fusion",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
    "2.2": {
        "title": "Index maintenance: consistency, forced rebuild, interrupted updates",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
    "2.4": {
        "title": "Context packages: token budget, evidence resolution, warnings, no duplicates",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "mcp/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs", "mcp"],
        },
    },
    "2.7": {
        "title": "Observability: sampled query log, audit log, metrics, retention",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "mcp/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs", "mcp"],
        },
    },
    "2.6": {
        "title": "Multi-agent coordination: visibility, write rights, approval, concurrency",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "mcp/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs", "mcp"],
        },
    },
    "2.5": {
        "title": "MCP server: nine tools over the local API, end-to-end with the MCP SDK client",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "mcp/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs", "mcp"],
        },
    },
    "2.3": {
        "title": "Evidence-aware invalidation: revalidation, accept, rename and move tracking",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
    "1.6": {
        "title": "Read-only web UI served by the daemon",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
            "web/src/*", "web/package.json", "web/package-lock.json", "web/index.html",
            "web/vite.config.ts", "web/tsconfig.json",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
            # The UI is built and tested with the pinned lockfile. Run locally:
            # the CI sandbox has no Node toolchain or npm registry access.
            "web": ["bash", "-c", "cd web && npm ci --no-audit --no-fund && npm run build && npx vitest run"],
        },
    },
    "1.5": {
        "title": "Daemon, CLI with JSON output and exit codes, single-writer lock",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
    "1.4": {
        "title": "Agent memory: revisions, transitions, evidence, export and import",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
    "3.1": {
        "title": "Web interface: retrieval inspector, memory timeline, visual code graph; browser tests",
        "sources": [
            "CMakeLists.txt", "cmake/*", "include/*", "src/*", "web/*", "scripts/ci.sh",
            "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "web", "certs"],
        },
    },
    "3.2": {
        "title": "Performance: benchmark matrix at three corpus sizes, variance across runs, manifest",
        "sources": [
            "CMakeLists.txt", "cmake/*", "include/*", "src/*", "scripts/bench.py",
            "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
            "docs/benchmarks/*",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "bench", "certs"],
        },
    },
    "3.4": {
        "title": "Packaging: container files, checked backup and restore, end-to-end install-to-restore",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*", "tests/*",
            "mcp/*", "scripts/*", "Dockerfile", "docker-compose.yml", ".dockerignore",
            "docs/OPERATIONS.md",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "certs", "mcp", "e2e", "container"],
        },
    },
    "3.6a": {
        "title": "Offline evaluation: labelled retrieval and stale-detection gates, no LLM",
        "sources": [
            "CMakeLists.txt", "cmake/*", "include/*", "src/*", "evaluation/*",
            "scripts/evaluate.py", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "eval", "certs"],
        },
    },
    "3.3": {
        "title": "Security: traversal and isolation tests, fuzzing with sanitizers, history scan",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "mcp/*", "docs/SECURITY.md", "scripts/ci.sh", "scripts/certify.py",
            "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs", "mcp", "fuzz"],
        },
    },
    "1.3b": {
        "title": "Code graph (JavaScript, TypeScript): per-language labelled precision and recall",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
    "1.3a": {
        "title": "Code graph (Python): tree-sitter symbols, facts, resolved edges",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/corpus.sh", "scripts/certify.py",
            "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
    "1.2": {
        "title": "Repository ingestion: git-aware listing, classification, batching",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/corpus.sh", "scripts/certify.py",
            "scripts/verify_certs.py",
        ],
        "mandatory": {
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
    "1.1": {
        "title": "Vertical slice: runtime, SQLite schema, Python indexer, CLI",
        "sources": [
            "CMakeLists.txt", ".clang-format", "cmake/*", "include/*", "src/*",
            "tests/*", "scripts/ci.sh", "scripts/certify.py", "scripts/verify_certs.py",
        ],
        "mandatory": {
            # One sandbox session: format, gcc, clang, sanitizers, and the
            # certification verifier. Unit, integration and CLI tests all run
            # under the gcc, clang and san jobs via ctest.
            "ci": ["bash", "scripts/ci.sh", "format", "gcc", "clang", "san", "certs"],
        },
    },
}


def git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()


def die(msg: str) -> None:
    print(f"certify: {msg}", file=sys.stderr)
    sys.exit(2)


def module_blobs(commit: str, patterns: list[str]) -> dict[str, str]:
    blobs = {}
    for line in git("ls-tree", "-r", commit).splitlines():
        meta, path = line.split("\t", 1)
        if any(fnmatch.fnmatchcase(path, p) for p in patterns):
            blobs[path] = meta.split()[2]
    return dict(sorted(blobs.items()))


def manifest_digest(manifest: dict) -> str:
    body = {k: v for k, v in manifest.items() if k != "manifest_sha256"}
    return hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()


def resolve(cmd: list[str]) -> list[str]:
    # On Windows, CreateProcess searches System32 before PATH, so a bare "bash"
    # starts WSL's launcher, not Git Bash. Resolve the executable explicitly.
    if cmd[0] == "bash":
        found = shutil.which("bash")
        if found is None:
            die("bash not found on PATH; install Git for Windows or run from a Linux host")
        cmd = [found, *cmd[1:]]
    return cmd


def run_check(name: str, cmd: list[str], logdir: pathlib.Path) -> dict:
    cmd = resolve(cmd)
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
    text = proc.stdout + proc.stderr
    (logdir / f"{name}.log").write_text(text, encoding="utf-8")
    result = {
        "name": name,
        "cmd": " ".join(cmd),
        "exit": proc.returncode,
        "log": f"logs/{name}.log",
    }
    # ctest prints "N% tests passed, F tests failed out of T" per run; sum runs.
    summaries = re.findall(r"tests failed out of (\d+)", text)
    failures = re.findall(r"(\d+) tests? failed out of", text)
    if summaries:
        total = sum(int(t) for t in summaries)
        failed = sum(int(f) for f in failures)
        result.update(tests_run=total, failed=failed, passed=total - failed)
    return result


def certify(module: str) -> int:
    spec = MODULES.get(module) or die(f"unknown module {module!r}; known: {sorted(MODULES)}")

    if git("status", "--porcelain"):
        die("working tree is dirty; commit first")
    sha = git("rev-parse", "HEAD")
    try:
        upstream = git("rev-parse", "@{u}")
    except subprocess.CalledProcessError:
        die("branch has no upstream; push first")
    if upstream != sha:
        die("HEAD is not pushed; CI tests the pushed ref, so push before certifying")

    out = CERT_DIR / module / sha
    logs = out / "logs"
    logs.mkdir(parents=True, exist_ok=True)

    checks = [run_check(n, c, logs) for n, c in spec["mandatory"].items()]
    exits = [c["exit"] for c in checks]
    if all(e == 0 for e in exits):
        status = "PASS"
    elif any(e == 0 for e in exits):
        status = "PARTIAL"
    else:
        status = "FAIL"

    manifest = {
        "module": module,
        "title": spec["title"],
        "commit": sha,
        "tree_dirty": False,
        "module_blobs": module_blobs(sha, spec["sources"]),
        "checks": checks,
        "status": status,
        "generated_by": "scripts/certify.py",
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    }
    manifest["manifest_sha256"] = manifest_digest(manifest)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(f"module {module} @ {sha[:12]}: {status}")
    for c in checks:
        extra = f" ({c['passed']}/{c['tests_run']} tests)" if "tests_run" in c else ""
        print(f"  {'PASS' if c['exit'] == 0 else 'FAIL'}  {c['name']} exit={c['exit']}{extra}")
    print(f"manifest: {(out / 'manifest.json').relative_to(ROOT).as_posix()}")
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        die("usage: certify.py <module>")
    sys.exit(certify(sys.argv[1]))
