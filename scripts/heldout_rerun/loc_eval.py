#!/usr/bin/env python3
"""Offline file-localisation benchmark on SWE-bench Lite. No language model is called.

For each real GitHub issue: check out the repository at the issue's base commit, index it with
Veylqora, then rank files for the issue text and see where the gold file (the file the real fix
changed) lands. Compares Veylqora (lexical, vector, hybrid) with non-LLM baselines (BM25 over files
and an IDF-weighted identifier match, which mimics what a grep-using agent would do).

  python3 loc_eval.py DATASET.json OUT.jsonl [--ids IDS.txt] [--repo owner/name] [--limit N]

DATASET.json is the public SWE-bench Lite test split; IDS.txt is an optional list
of instance IDs (one per line) selecting which instances of it to run.

Resumable: instances already in OUT.jsonl are skipped. Run inside the sandbox.
"""
import argparse
import collections
import hashlib
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HOME = pathlib.Path.home()
VEYLQORA = pathlib.Path(os.environ.get("VEYLQORA_BIN", HOME / "ci-work" / "build-gcc" / "src" / "veylqora"))
WORK = pathlib.Path(os.environ.get("LOC_EVAL_WORK", HOME / "loc"))
MAX_QUERY = 512
TEST_PATH = re.compile(r"(^|/)(tests?|testing)(/|$)|(^|/)test_[^/]*\.py$|_tests?\.py$|(^|/)conftest\.py$")
STOP = {"the", "and", "for", "that", "this", "with", "not", "are", "when", "from", "have", "you", "but",
        "should", "would", "could", "can", "will", "has", "was", "None", "True", "False", "self", "import"}


def sh(cmd, cwd=None, timeout=3600):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True, timeout=timeout).stdout


# ---------- tokenising and baselines ----------

def split_tokens(text):
    """Same rule as ADR-0015: split on non-alphanumerics and camelCase boundaries, lower-case, keep >= 2 chars."""
    out = []
    for word in re.findall(r"[A-Za-z0-9]+", text):
        spaced = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", word)
        spaced = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", spaced)
        out.extend(p.lower() for p in spaced.split() if len(p) >= 2)
    return out


def keywords(text):
    """Identifier-like terms an engineer (or a grepping agent) would pull out of an issue."""
    found = []

    def add(token):
        token = token.strip(".,:;()[]{}'\"`")
        if len(token) >= 3 and token not in found and token not in STOP:
            found.append(token)

    for m in re.finditer(r"`([^`\n]{2,80})`", text):
        for tok in re.findall(r"[A-Za-z_][A-Za-z0-9_.]*", m.group(1)):
            add(tok)
    patterns = [r"\b[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+\b", r"\b[A-Z][a-z0-9]+(?:[A-Z][a-z0-9]+)+\b",
                r"\b[a-z0-9]+(?:_[a-z0-9]+)+\b", r"\b\w+(?:Error|Exception|Warning)\b", r"\b[\w/.-]+\.py\b"]
    for pattern in patterns:
        for m in re.finditer(pattern, text):
            add(m.group(0))
    query = ""
    for tok in found:
        if len(query) + len(tok) + 1 > MAX_QUERY:
            break
        query = (query + " " + tok).strip()
    return query


def title(text):
    for line in text.splitlines():
        if line.strip():
            return line.strip()[:200]
    return ""


def queries_for(issue):
    return {"first512": issue[:MAX_QUERY], "keywords": keywords(issue), "title": title(issue)}


class FileIndex:
    """Per-checkout file statistics, cached by blob id so unchanged files are read once."""
    cache = {}

    def __init__(self, repo_dir):
        listing = sh(["git", "ls-files", "-s", "--", "*.py"], cwd=repo_dir)
        self.paths, self.tokens, self.idents, self.lengths = [], [], [], []
        for line in listing.splitlines():
            meta, path = line.split("\t", 1)
            mode, sha = meta.split()[:2]
            if mode == "120000":
                continue
            if sha not in FileIndex.cache:
                try:
                    text = (repo_dir / path).read_text(encoding="utf-8", errors="ignore")
                except OSError:
                    text = ""
                toks = split_tokens(text)
                FileIndex.cache[sha] = (collections.Counter(toks), set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", text)), len(toks))
            counter, idents, length = FileIndex.cache[sha]
            self.paths.append(path)
            self.tokens.append(counter)
            self.idents.append(idents)
            self.lengths.append(max(length, 1))
        self.n = len(self.paths)
        self.avgdl = sum(self.lengths) / max(self.n, 1)

    def bm25(self, query, k1=1.5, b=0.75):
        qtokens = set(split_tokens(query))
        df = {t: sum(1 for c in self.tokens if t in c) for t in qtokens}
        scores = []
        for i, counter in enumerate(self.tokens):
            s = 0.0
            for t in qtokens:
                tf = counter.get(t, 0)
                if tf:
                    idf = math.log(1 + (self.n - df[t] + 0.5) / (df[t] + 0.5))
                    s += idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * self.lengths[i] / self.avgdl))
            scores.append(s)
        return self._rank(scores)

    def ident_match(self, query):
        """Sum of IDF of the query identifiers that appear verbatim in the file (an agent grepping for them)."""
        words = []
        for tok in query.split():
            for part in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", tok):
                if len(part) >= 3 and part not in words:
                    words.append(part)
        df = {w: sum(1 for ids in self.idents if w in ids) for w in words}
        scores = []
        for ids in self.idents:
            scores.append(sum(math.log(1 + self.n / df[w]) for w in words if df[w] and w in ids))
        return self._rank(scores)

    def _rank(self, scores):
        order = sorted(range(self.n), key=lambda i: (-scores[i], self.paths[i]))
        return [self.paths[i] for i in order if scores[i] > 0]


# ---------- Veylqora ----------

class Daemon:
    def __init__(self, db):
        self.log = open(str(db) + ".log", "w+")
        self.proc = subprocess.Popen([str(VEYLQORA), "serve", "--db", str(db), "--port", "0"],
                                     stdout=self.log, stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 30
        first = ""
        while time.monotonic() < deadline and self.proc.poll() is None:
            self.log.seek(0)
            first = self.log.readline()
            if first.endswith("\n"):
                break
            time.sleep(0.05)
        if not first.startswith("listening on http://127.0.0.1:"):
            raise RuntimeError(f"daemon did not start: {first!r}")
        self.url = first.split()[2].rstrip()

    def index(self, root):
        request = urllib.request.Request(self.url + "/api/v1/index", data=json.dumps({"root": str(root)}).encode(),
                                         method="POST", headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=3600) as response:
            return json.loads(response.read().decode())

    def retrieve(self, root, query, mode):
        if not split_tokens(query):
            return []
        params = urllib.parse.urlencode({"root": str(root), "query": query[:MAX_QUERY], "k": 100, "mode": mode})
        try:
            with urllib.request.urlopen(self.url + "/api/v1/retrieve?" + params, timeout=120) as response:
                hits = json.loads(response.read().decode())["hits"]
        except urllib.error.HTTPError:
            return []
        files = []
        for hit in hits:
            if hit["path"] not in files:
                files.append(hit["path"])
        return files

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


def rank_of(ranked, gold, drop_tests=False):
    items = [p for p in ranked if not (drop_tests and TEST_PATH.search(p))]
    return items.index(gold) + 1 if gold in items else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dataset")
    parser.add_argument("out")
    parser.add_argument("--repo", nargs="+")
    parser.add_argument("--ids", help="file with one instance ID per line; only those instances run")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--stride", type=int, default=1, help="take every Nth instance of each repo")
    parser.add_argument("--offset", type=int, default=0)
    args = parser.parse_args()
    if not VEYLQORA.is_file():
        sys.exit(f"veylqora binary not found at {VEYLQORA}; set VEYLQORA_BIN")
    data = json.load(open(args.dataset, encoding="utf-8"))
    if args.ids:
        wanted = {line.strip() for line in open(args.ids, encoding="utf-8") if line.strip()}
        data = [row for row in data if row["instance_id"] in wanted]
        missing = wanted - {row["instance_id"] for row in data}
        if missing:
            sys.exit(f"{len(missing)} instance ID(s) not in the dataset, e.g. {sorted(missing)[:3]}")
    out_path = pathlib.Path(args.out)
    done = {json.loads(line)["instance_id"] for line in out_path.read_text().splitlines()} if out_path.exists() else set()
    WORK.mkdir(exist_ok=True)
    by_repo = collections.defaultdict(list)
    for row in data:
        if args.repo and row["repo"] not in args.repo:
            continue
        by_repo[row["repo"]].append(row)
    manifest = {"started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "veylqora_sha256": hashlib.sha256(VEYLQORA.read_bytes()).hexdigest(),
                "max_query_chars": MAX_QUERY, "k": 100}
    print(json.dumps(manifest), flush=True)
    count = 0
    for repo, rows in sorted(by_repo.items(), key=lambda kv: len(kv[1])):
        rows = sorted(rows, key=lambda r: r["instance_id"])
        rows = rows[args.offset::args.stride]
        todo = [r for r in rows if r["instance_id"] not in done]
        if args.limit:
            todo = todo[: max(args.limit - count, 0)]
        if not todo:
            continue
        repo_dir = WORK / repo.replace("/", "__")
        if not repo_dir.exists():
            sh(["git", "clone", "-q", "--filter=blob:none", f"https://github.com/{repo}", str(repo_dir)], timeout=3600)
        if True:
            for row in todo:
                started = time.monotonic()
                gold = re.findall(r"^diff --git a/(\S+) b/", row["patch"], re.M)[0]
                sh(["git", "checkout", "-q", "-f", "--detach", row["base_commit"]], cwd=repo_dir, timeout=3600)
                files = FileIndex(repo_dir)
                db = WORK / (repo.replace("/", "__") + ".db")
                for suffix in ("", "-wal", "-shm", ".lock", ".log"):
                    pathlib.Path(str(db) + suffix).unlink(missing_ok=True)
                daemon = Daemon(db)
                t_index = time.monotonic()
                indexed = daemon.index(repo_dir)
                index_s = time.monotonic() - t_index
                issue = row["problem_statement"]
                qs = queries_for(issue)
                ranked = {}
                for qname, q in qs.items():
                    for mode in ("lexical", "vector", "hybrid"):
                        ranked[f"veylqora_{mode}_{qname}"] = daemon.retrieve(repo_dir, q, mode)
                    ranked[f"bm25_{qname}"] = files.bm25(q)
                ranked["bm25_full"] = files.bm25(issue)
                ranked["identmatch_keywords"] = files.ident_match(qs["keywords"])
                record = {"instance_id": row["instance_id"], "repo": repo, "gold": gold,
                          "gold_is_candidate": gold in files.paths, "n_py_files": files.n,
                          "veylqora_indexed": indexed.get("indexed"), "index_seconds": round(index_s, 2),
                          "keywords_query": qs["keywords"],
                          "rank_all": {m: rank_of(r, gold) for m, r in ranked.items()},
                          "rank_nontest": {m: rank_of(r, gold, True) for m, r in ranked.items()},
                          "list_len": {m: len(r) for m, r in ranked.items()}}
                with out_path.open("a", encoding="utf-8") as handle:
                    handle.write(json.dumps(record) + "\n")
                count += 1
                print(f"{row['instance_id']} gold_candidate={record['gold_is_candidate']} hybrid_first512={record['rank_all']['veylqora_hybrid_first512']} "
                      f"bm25_first512={record['rank_all']['bm25_first512']} index={index_s:.1f}s total={time.monotonic() - started:.1f}s", flush=True)
                daemon.stop()
    print("DONE", count, flush=True)


if __name__ == "__main__":
    main()
