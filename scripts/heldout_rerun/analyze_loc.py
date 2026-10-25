"""Analyse the offline localisation run: accuracy at k with bootstrap CIs, paired comparisons, per repo."""
import collections
import json
import random
import sys

random.seed(7)
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
rows = [r for r in rows if r["gold_is_candidate"]]
print(f"instances: {len(rows)}  (gold file not among candidate .py files, excluded: see below)")
methods = sorted(rows[0]["rank_all"])


def acc(values, k):
    return sum(1 for v in values if v is not None and v <= k) / len(values)


def mrr(values):
    return sum(1 / v for v in values if v) / len(values)


def ci(values, fn, n=2000):
    stats = sorted(fn([random.choice(values) for _ in values]) for _ in range(n))
    return stats[int(0.025 * n)], stats[int(0.975 * n)]


def table(kind):
    print(f"\n=== file ranking, {kind} ===")
    print(f"{'method':32s} {'@1':>6s} {'@3':>6s} {'@5':>6s} {'@10':>6s} {'MRR':>6s}  @5 95% CI       not found")
    for m in methods:
        vals = [r[kind][m] for r in rows]
        lo, hi = ci(vals, lambda v: acc(v, 5))
        print(f"{m:32s} {acc(vals,1):6.3f} {acc(vals,3):6.3f} {acc(vals,5):6.3f} {acc(vals,10):6.3f} {mrr(vals):6.3f}  [{lo:.2f},{hi:.2f}]   {sum(1 for v in vals if v is None)/len(vals):.2f}")


table("rank_all")
table("rank_nontest")


def paired(a, b, kind, k):
    diffs = [(1 if (r[kind][a] is not None and r[kind][a] <= k) else 0) - (1 if (r[kind][b] is not None and r[kind][b] <= k) else 0) for r in rows]
    mean = sum(diffs) / len(diffs)
    lo, hi = ci(diffs, lambda d: sum(d) / len(d))
    wins = sum(1 for d in diffs if d > 0)
    losses = sum(1 for d in diffs if d < 0)
    return mean, lo, hi, wins, losses


print("\n=== paired differences in accuracy@k (A minus B), non-test files, 95% bootstrap CI over instances ===")
pairs = [("veylqora_hybrid_first512", "bm25_first512"), ("veylqora_hybrid_keywords", "bm25_keywords"),
         ("veylqora_hybrid_keywords", "identmatch_keywords"), ("veylqora_hybrid_title", "bm25_title"),
         ("veylqora_hybrid_keywords", "veylqora_lexical_keywords"), ("veylqora_hybrid_keywords", "veylqora_vector_keywords"),
         ("veylqora_lexical_first512", "bm25_first512")]
for a, b in pairs:
    for k in (1, 5, 10):
        mean, lo, hi, w, l = paired(a, b, "rank_nontest", k)
        print(f"  {a} - {b} @{k}: {mean:+.3f} [{lo:+.3f},{hi:+.3f}]  wins {w} losses {l}")

print("\n=== per repo, non-test @5: veylqora_hybrid_keywords / bm25_keywords / bm25_full (n) ===")
by = collections.defaultdict(list)
for r in rows:
    by[r["repo"]].append(r)
for repo, rs in sorted(by.items(), key=lambda kv: -len(kv[1])):
    fm = lambda m: acc([r["rank_nontest"][m] for r in rs], 5)
    print(f"  {repo:28s} {fm('veylqora_hybrid_keywords'):.2f} / {fm('bm25_keywords'):.2f} / {fm('bm25_full'):.2f}   (n={len(rs)}, files median {sorted(r['n_py_files'] for r in rs)[len(rs)//2]})")

print("\n=== index time per instance (s): median / max ===")
times = sorted(r["index_seconds"] for r in rows)
print(f"  {times[len(times)//2]:.1f} / {times[-1]:.1f}")
