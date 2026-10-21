#!/usr/bin/env python3
"""Performance benchmarks (M3.2): a deterministic generated corpus at several
sizes, measured through the running daemon's HTTP API.

Records, per corpus size: cold index time, incremental index time after a 1%
change, retrieval latency (p50/p95/p99) for each mode, name-search latency, the
daemon's resident memory and CPU time, and a manifest of the machine, the build
and the dataset. Results are written to docs/benchmarks/<name>/results.json.

    VEYLQORA_BIN=build/src/veylqora python3 scripts/bench.py --out docs/benchmarks/run
"""

import argparse
import hashlib
import json
import os
import pathlib
import platform
import random
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIZES = (200, 1000, 5000)       # Python files in the generated corpus
WARMUP = 20                     # queries per mode before measuring
REPS = 200                      # measured queries per mode
QUERIES = [
    "compute total", "parse record", "format value", "load config",
    "validate input", "merge items", "render page", "count entries",
]
MODES = ("lexical", "vector", "hybrid")


def generate_corpus(root, files, seed=7):
    """Writes `files` Python modules: 5 functions each, with calls between them."""
    rng = random.Random(seed)
    digest = hashlib.sha256()
    names = ["compute", "parse", "format", "load", "validate", "merge", "render", "count"]
    for i in range(files):
        package = root / f"pkg{i // 50:03d}"
        package.mkdir(parents=True, exist_ok=True)
        lines = ["import os", ""]
        for f in range(5):
            name = f"{rng.choice(names)}_{i}_{f}"
            callee = f"{rng.choice(names)}_{rng.randrange(files)}_{rng.randrange(5)}"
            lines += [f"def {name}(value):",
                      f"    # generated for the benchmark",
                      f"    return {callee}(value) + len(str(value))", ""]
        text = "\n".join(lines) + "\n"
        (package / f"mod{i % 50:03d}_{i}.py").write_text(text, encoding="utf-8")
        digest.update(text.encode("utf-8"))
    return digest.hexdigest()


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    index = min(len(sorted_values) - 1, int(round(p / 100.0 * (len(sorted_values) - 1))))
    return sorted_values[index]


def timed_get(url, path, query):
    full = url + path + "?" + urllib.parse.urlencode(query)
    start = time.perf_counter()
    with urllib.request.urlopen(full) as response:
        response.read()
    return (time.perf_counter() - start) * 1000.0


def post(url, path, body):
    data = json.dumps(body).encode()
    request = urllib.request.Request(url + path, data=data, method="POST",
                                     headers={"Content-Type": "application/json"})
    start = time.perf_counter()
    with urllib.request.urlopen(request) as response:
        json.loads(response.read().decode())
    return time.perf_counter() - start


def process_stats(pid):
    """Resident memory (kB) and CPU time (s) of the daemon, from /proc (Linux)."""
    rss_kb = 0
    status = pathlib.Path(f"/proc/{pid}/status")
    if status.exists():
        for line in status.read_text().splitlines():
            if line.startswith("VmRSS:"):
                rss_kb = int(line.split()[1])
    cpu_seconds = None
    stat = pathlib.Path(f"/proc/{pid}/stat")
    if stat.exists():
        fields = stat.read_text().rsplit(")", 1)[1].split()
        ticks = os.sysconf("SC_CLK_TCK")
        cpu_seconds = (int(fields[11]) + int(fields[12])) / ticks
    return rss_kb, cpu_seconds


def start_daemon(binary, db, port):
    proc = subprocess.Popen([binary, "serve", "--db", str(db), "--port", str(port)],
                            stdout=subprocess.PIPE, text=True)
    line = proc.stdout.readline()
    if not line.startswith("listening on http://127.0.0.1:"):
        proc.kill()
        sys.exit(f"daemon did not start: {line!r}")
    return proc, line.split()[2].rstrip()


def measure_size(binary, files, work, runs):
    results = []
    corpus_sha = None
    for run in range(runs):
        # A fresh, identical corpus per run: the incremental step edits files, and
        # a later run must not start from an edited tree.
        corpus = work / f"corpus-{files}-{run}"
        corpus.mkdir()
        corpus_sha = generate_corpus(corpus, files)
        db = work / f"bench-{files}-{run}.db"
        proc, url = start_daemon(binary, db, 0)
        try:
            cold_start = time.perf_counter()
            post(url, "/api/v1/index", {"root": str(corpus)})
            cold_index = time.perf_counter() - cold_start

            # Change 1% of the files (at least one) and time the incremental run.
            changed = max(1, files // 100)
            for i in range(changed):
                path = corpus / f"pkg{i // 50:03d}" / f"mod{i % 50:03d}_{i}.py"
                path.write_text(path.read_text(encoding="utf-8") + "\n# touched\n",
                                encoding="utf-8")
            incremental_start = time.perf_counter()
            post(url, "/api/v1/index", {"root": str(corpus)})
            incremental_index = time.perf_counter() - incremental_start

            latencies = {}
            for mode in MODES:
                for q in QUERIES[:4] * (WARMUP // 4):
                    timed_get(url, "/api/v1/retrieve", {"root": str(corpus), "query": q,
                                                         "k": 10, "mode": mode})
                samples = []
                for i in range(REPS):
                    q = QUERIES[i % len(QUERIES)]
                    samples.append(timed_get(url, "/api/v1/retrieve",
                                             {"root": str(corpus), "query": q, "k": 10,
                                              "mode": mode}))
                samples.sort()
                latencies[mode] = {"p50": percentile(samples, 50),
                                   "p95": percentile(samples, 95),
                                   "p99": percentile(samples, 99)}

            name_samples = []
            for i in range(REPS):
                name_samples.append(timed_get(url, "/api/v1/search",
                                              {"root": str(corpus), "name": f"compute_{i}_0"}))
            name_samples.sort()
            rss_kb, cpu_seconds = process_stats(proc.pid)
            results.append({
                "run": run,
                "cold_index_seconds": cold_index,
                "incremental_index_seconds": incremental_index,
                "files_changed": changed,
                "retrieve_ms": latencies,
                "name_search_ms": {"p50": percentile(name_samples, 50),
                                   "p95": percentile(name_samples, 95),
                                   "p99": percentile(name_samples, 99)},
                "daemon_rss_kb": rss_kb,
                "daemon_cpu_seconds": cpu_seconds,
            })
        finally:
            proc.terminate()
            proc.wait(timeout=20)
        shutil.rmtree(work / f"bench-{files}-{run}.db", ignore_errors=True)
        shutil.rmtree(corpus, ignore_errors=True)
    return {"files": files, "dataset_sha256": corpus_sha, "runs": results}


def cpu_model():
    cpuinfo = pathlib.Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def manifest(binary):
    compiler = subprocess.run(["sh", "-c", "c++ --version | head -1"], capture_output=True,
                              text=True).stdout.strip()
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_model": cpu_model(),
        "cpu_count": os.cpu_count(),
        "compiler": compiler,
        "binary": binary,
        "binary_sha256": hashlib.sha256(pathlib.Path(binary).read_bytes()).hexdigest(),
        "python": sys.version.split()[0],
        "sizes": list(SIZES),
        "warmup_queries_per_mode": WARMUP,
        "measured_queries_per_mode": REPS,
        "modes": list(MODES),
        "query_set": QUERIES,
        "dataset": "generated by scripts/bench.py (seed 7); see dataset_sha256 per size",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=os.environ.get("VEYLQORA_BIN"))
    parser.add_argument("--out", default=str(ROOT / "docs" / "benchmarks" / "run"))
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    if not args.bin:
        sys.exit("set VEYLQORA_BIN or pass --bin")

    work = pathlib.Path(tempfile.mkdtemp(prefix="veylqora-bench-"))
    try:
        sizes = [measure_size(args.bin, files, work, args.runs) for files in SIZES]
    finally:
        shutil.rmtree(work, ignore_errors=True)

    report = {"manifest": manifest(args.bin), "sizes": sizes}
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for entry in sizes:
        hybrid = [run["retrieve_ms"]["hybrid"]["p50"] for run in entry["runs"]]
        print(f"{entry['files']} files: cold index "
              f"{entry['runs'][0]['cold_index_seconds']:.2f}s, hybrid p50 "
              f"{min(hybrid):.2f}-{max(hybrid):.2f} ms over {len(hybrid)} runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
