#!/usr/bin/env python3
"""Offline evaluation (M3.6a): retrieval quality and stale detection against the
committed labelled datasets. No LLM, no network beyond the local daemon.

It starts a real daemon, indexes the fixture through the HTTP API, and writes
results to evaluation/results.json. The gates in GATES are fixed in
evaluation/README.md before the first scored run; they are not tuned to results.

    VEYLQORA_BIN=build/src/veylqora python3 scripts/evaluate.py
"""

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
DATASETS = ROOT / "evaluation" / "datasets"

# Fixed before the first scored run (see evaluation/README.md).
GATES = {
    "retrieval_hybrid_recall_at_10": 0.90,  # M2.1 gate
    "stale_precision": 0.80,                # ADR-0014
    "stale_recall": 0.70,                   # ADR-0014
}
FLAGGED_STATES = {"suspected_stale", "evidence_missing"}


def start_daemon(binary, work):
    proc = subprocess.Popen(
        [binary, "serve", "--db", str(work / "eval.db"), "--port", "0"],
        stdout=subprocess.PIPE, text=True)
    line = proc.stdout.readline()
    if not line.startswith("listening on http://127.0.0.1:"):
        proc.kill()
        sys.exit(f"daemon did not start: {line!r}")
    return proc, line.split()[2].rstrip()


def call(url, method, path, query=None, body=None):
    if query:
        path += "?" + urllib.parse.urlencode(query)
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(url + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request) as response:
        return json.loads(response.read().decode())


def apply_ops(root, ops):
    """Applies one step of edits. A replace must match exactly once, so a typo in a
    dataset fails loudly instead of silently changing nothing."""
    for op in ops:
        path = root / op["path"]
        if op["op"] == "replace":
            text = path.read_bytes().decode("utf-8")
            if text.count(op["old"]) != 1:
                raise ValueError(f"{op['path']}: replace target matched {text.count(op['old'])} times")
            path.write_bytes(text.replace(op["old"], op["new"]).encode("utf-8"))
        elif op["op"] == "write":
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(op["content"].encode("utf-8"))
        elif op["op"] == "append":
            path.parent.mkdir(parents=True, exist_ok=True)
            previous = path.read_bytes().decode("utf-8") if path.exists() else ""
            path.write_bytes((previous + op["content"]).encode("utf-8"))
        else:
            raise ValueError(f"unknown op: {op['op']}")


def evaluate_retrieval(url, root, dataset):
    rows = []
    for item in dataset["queries"]:
        relevant = set(item["relevant"])
        row = {"query": item["query"], "relevant": sorted(relevant)}
        for mode in ("hybrid", "lexical", "vector"):
            hits = call(url, "GET", "/api/v1/retrieve", {
                "root": str(root), "query": item["query"], "k": 10, "mode": mode})["hits"]
            top = [h["fqn"] for h in hits[:10]]
            row[f"{mode}_recall_at_10"] = len(relevant & set(top)) / len(relevant)
            if mode == "hybrid":
                first = next((i + 1 for i, fqn in enumerate(top) if fqn in relevant), None)
                row["hybrid_reciprocal_rank"] = 0.0 if first is None else 1.0 / first
                row["hybrid_top_3"] = top[:3]
        rows.append(row)
    summary = {}
    for key in ("hybrid_recall_at_10", "lexical_recall_at_10", "vector_recall_at_10",
                "hybrid_reciprocal_rank"):
        summary[key] = sum(r[key] for r in rows) / len(rows)
    return rows, summary


def evaluate_staleness(url, work, dataset, fixtures):
    results = []
    tp = fp = fn = tn = 0
    for index, scenario in enumerate(dataset["scenarios"]):
        root = work / f"scenario-{index:02d}"
        shutil.copytree(fixtures, root)
        root_text = str(root)
        call(url, "POST", "/api/v1/index", body={"root": root_text})
        ids = {}
        for key, memory in dataset["memories"].items():
            created = call(url, "POST", "/api/v1/memories", body={
                "root": root_text, "type": "decision", "content": memory["content"],
                "author": "eval", "scope": "shared", "evidence": [memory["evidence"]]})
            ids[key] = created["id"]
        for step in scenario["steps"]:
            apply_ops(root, step)
            call(url, "POST", "/api/v1/index", body={"root": root_text})
        states = {}
        for key, memory_id in ids.items():
            detail = call(url, "GET", f"/api/v1/memories/{memory_id}",
                          {"root": root_text, "viewer": "eval"})
            states[key] = detail["state"]
        expected = set(scenario["expect_flagged"])
        for key in dataset["memories"]:
            predicted = states[key] in FLAGGED_STATES
            label = key in expected
            if label and predicted:
                tp += 1
            elif predicted:
                fp += 1
            elif label:
                fn += 1
            else:
                tn += 1
        results.append({
            "name": scenario["name"],
            "expected_flagged": sorted(expected),
            "states": states,
            "correct": all((states[k] in FLAGGED_STATES) == (k in expected)
                           for k in dataset["memories"]),
        })
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    counts = {"tp": tp, "fp": fp, "fn": fn, "tn": tn}
    return results, counts, precision, recall


def negative_results(summary, scenarios):
    notes = []
    modes = {m: summary[f"{m}_recall_at_10"] for m in ("hybrid", "lexical", "vector")}
    if all(value >= 1.0 for value in modes.values()):
        notes.append("all three retrieval modes reach mean recall@10 of 1.0 on this set, so "
                     "it cannot separate them; a harder set is needed before comparing modes")
    else:
        for name, value in modes.items():
            if value < 1.0:
                notes.append(f"{name} mean recall@10 is {value:.3f}")
    if summary["hybrid_reciprocal_rank"] < 1.0:
        notes.append(f"hybrid mean reciprocal rank is {summary['hybrid_reciprocal_rank']:.3f}: "
                     "some relevant definitions are not ranked first")
    for scenario in scenarios:
        if not scenario["correct"]:
            notes.append(f"stale scenario not as expected: {scenario['name']}")
    return notes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=os.environ.get("VEYLQORA_BIN"))
    parser.add_argument("--fixtures", default=str(ROOT / "tests" / "fixtures" / "python"))
    parser.add_argument("--out", default=str(ROOT / "evaluation" / "results.json"))
    args = parser.parse_args()
    if not args.bin:
        sys.exit("set VEYLQORA_BIN or pass --bin")

    retrieval_set = json.loads((DATASETS / "retrieval.json").read_text(encoding="utf-8"))
    staleness_set = json.loads((DATASETS / "staleness.json").read_text(encoding="utf-8"))
    fixtures = pathlib.Path(args.fixtures)

    work = pathlib.Path(tempfile.mkdtemp(prefix="veylqora-eval-"))
    proc, url = start_daemon(args.bin, work)
    try:
        index_root = work / "index-root"
        shutil.copytree(fixtures, index_root)
        call(url, "POST", "/api/v1/index", body={"root": str(index_root)})
        retrieval_rows, retrieval_summary = evaluate_retrieval(url, index_root, retrieval_set)
        scenarios, counts, precision, recall = evaluate_staleness(
            url, work / "stale", staleness_set, fixtures)
    finally:
        proc.terminate()
        proc.wait(timeout=10)
        shutil.rmtree(work, ignore_errors=True)

    measured = {
        "retrieval_hybrid_recall_at_10": retrieval_summary["hybrid_recall_at_10"],
        "stale_precision": precision,
        "stale_recall": recall,
    }
    gates = {name: {"threshold": GATES[name], "value": measured[name],
                    "passed": measured[name] >= GATES[name]} for name in GATES}
    report = {
        "gates": gates,
        "passed": all(g["passed"] for g in gates.values()),
        "retrieval": {"summary": retrieval_summary, "queries": retrieval_rows},
        "stale_detection": {"counts": counts, "precision": precision, "recall": recall,
                            "scenarios": scenarios},
        # Negative results are recorded even when they are not gated. Each note is
        # written only when the numbers show it, so the report cannot claim a gap
        # that is not there.
        "negative_results": negative_results(retrieval_summary, scenarios),
    }
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    for name, gate in gates.items():
        mark = "PASS" if gate["passed"] else "FAIL"
        print(f"{mark}  {name}: {gate['value']:.3f} (threshold {gate['threshold']:.2f})")
    print(f"stale counts: {counts}")
    for entry in report["negative_results"]:
        print(f"note: {entry}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
