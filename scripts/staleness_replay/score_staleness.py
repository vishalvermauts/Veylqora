#!/usr/bin/env python3
"""Scorer for the Veylqora memory-staleness replay.

Committed together with the other study scripts BEFORE any Veylqora build or
run. It consumes cases.json plus the raw replay rows (steps.jsonl,
cases.jsonl) and nothing else. CPU-only: no AI, network or veylqora calls.

SCORING RULE (verbatim; fixed before running and restated in scripts/
staleness_replay/README.md and docs/research/staleness-replay-summary.md):

  A case is predicted_positive if veylqora ever reports the state
  suspected_stale or evidence_missing for its memory at any replay step
  1..min(K, horizon) of its group. superseded/invalidated/archived are
  non-positive. Ground truth: type a (body_change) and type e (deleted)
  are truth positive; types b (unchanged_neighbours_change), c
  (renamed), d (moved), f (callee_only_changed) are truth negative (the
  memory must stay active; an active memory that was re-anchored after a
  rename or move is a correct non-flag).

  CORE POOL = types a + b. PASS RULE: "precision >= 0.85 and recall >=
  0.85 on the body-change cases". precision = predicted positives whose
  truth is positive, divided by all predicted positives of the core pool;
  recall = predicted positives among type-a cases, divided by all
  type-a cases. Cases whose `memory add` failed are EXCLUDED from every
  denominator and reported separately. The same numbers are recomputed
  over 10,000 bootstrap resamples drawn with random.Random(20261024) to
  give 95% percentile confidence intervals for precision and recall.

  SPECIALS are only tabulated, never merged into PASS RULE numbers: for
  renamed and moved the end-of-window state versus expectation "active",
  for deleted versus "evidence_missing", for callee_only versus
  "active"; deviations are listed per case.

  For false positives (type b flagged) and false negatives (type a not
  flagged) the scorer re-parses the cited definition's file version at
  t0 and at the first observably differing step that veylqora acted on
  (byte spans from `git show <sha>:<path>`; helpers imported from
  label_history.py) and classifies the deviation into exactly one of
  these categories: signature-only, docstring-only, decorator-only,
  annotation-only, comment-adjacent, whitespace/blank-line-only, other,
  not-found, not-parsable, no-change-observed. The classification names
  evidence, not an excuse; every category is reported with the case ids
  it covers.

CLI: python3 score_staleness.py --cases <cases.json> --steps <steps.jsonl>
     --cases-rows <cases.jsonl> --repos-root <dir> --lineages <dir>
     [--out <report.json>]
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from label_history import module_fqm, parse_version, sh  # noqa: E402

SEED = 20261024
K = 20
POSITIVE_STATES = ('suspected_stale', 'evidence_missing')
PASS_PRECISION = 0.85
PASS_RECALL = 0.85
BOOTSTRAP = 10000
CATEGORY_ORDER = ['signature-only', 'docstring-only', 'decorator-only',
                  'annotation-only', 'comment-adjacent',
                  'whitespace/blank-line-only', 'other', 'not-found',
                  'not-parsable', 'no-change-observed']


def truth_positive(t):
    return t in ('a', 'e')


def load_jsonl(path):
    rows = []
    with open(path, encoding='utf-8') as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def bucket_steps(rows):
    steps = {}
    for r in rows:
        steps.setdefault((r['repo'], r['t0']), []).append(r)
    return steps


def merge_memory_ids(cases_map, case_rows):
    for r in case_rows:
        c = cases_map.get(r['case_id'])
        if c is not None and 'memory_id' in r:
            c['memory_id'] = r['memory_id']


def per_case(steps, cases, add_failures):
    out = {}
    for cid, case in cases.items():
        if cid in add_failures or not case.get('memory_id'):
            out[cid] = {'excluded': 'add_failed'}
            continue
        tag = (case['repo'], case['t0'])
        flagged_at = None
        end_state = None
        sha_state = {}
        for r in steps.get(tag, []):
            if r['s'] > min(K, case['horizon']):
                continue
            st = (r.get('states') or {}).get(case['memory_id'])
            if st is not None:
                end_state = st
                sha_state[r['s']] = r['sha']
            if st in POSITIVE_STATES and flagged_at is None:
                flagged_at = r['s']
        tp = truth_positive(case['type'])
        out[cid] = {'case_id': cid, 'type': case['type'],
                    't0': case['t0'], 'horizon': case['horizon'],
                    'expect_flagged': case['expect_flagged'],
                    'expect_state': case['expect_state'],
                    'flagged_at': flagged_at, 'end_state': end_state,
                    'sha_state': sha_state,
                    'predicted_positive': flagged_at is not None,
                    'truth_positive': tp,
                    'correct': (flagged_at is not None) == tp}
    return out


def core_metrics(details, cases):
    a_ids = [cid for cid, d in details.items()
             if not d.get('excluded') and cases[cid]['type'] == 'a']
    b_ids = [cid for cid, d in details.items()
             if not d.get('excluded') and cases[cid]['type'] == 'b']
    tp = sum(1 for cid in a_ids if details[cid]['predicted_positive'])
    fp = sum(1 for cid in b_ids if details[cid]['predicted_positive'])
    fn = len(a_ids) - tp
    tn = len(b_ids) - fp
    precision = (tp / (tp + fp)) if (tp + fp) else None
    recall = (tp / len(a_ids)) if a_ids else None
    core = {'pool': {'a': len(a_ids), 'b': len(b_ids)},
            'tp': tp, 'fp': fp, 'fn': fn, 'tn': tn,
            'precision': precision, 'recall': recall,
            'pass_rule': 'precision >= 0.85 and recall >= 0.85 on the '
                         'body-change cases',
            'pass': precision is not None and recall is not None
            and precision >= PASS_PRECISION and recall >= PASS_RECALL}
    return core, a_ids, b_ids


def bootstrap_ci(a_ids, b_ids, details):
    rng = random.Random(SEED)
    a_flags = [details[cid]['predicted_positive'] for cid in a_ids]
    b_flags = [details[cid]['predicted_positive'] for cid in b_ids]
    if not a_flags or len(a_flags) + len(b_flags) < 2:
        return None
    precs, recs = [], []
    for _ in range(BOOTSTRAP):
        sa = [a_flags[rng.randrange(len(a_flags))]
              for _ in range(len(a_flags))]
        sb = [b_flags[rng.randrange(len(b_flags))]
              for _ in range(len(b_flags))]
        tp = sum(1 for x in sa if x)
        pp = tp + sum(1 for x in sb if x)
        precs.append((tp / pp) if pp else 0.0)
        recs.append((tp / len(sa)) if sa else 0.0)
    precs.sort()
    recs.sort()
    return {'precision_ci95': [precs[int(0.025 * BOOTSTRAP)],
                               precs[min(BOOTSTRAP - 1,
                                         int(0.975 * BOOTSTRAP))]],
            'recall_ci95': [recs[int(0.025 * BOOTSTRAP)],
                            recs[min(BOOTSTRAP - 1,
                                     int(0.975 * BOOTSTRAP))]]}


def specials_table(details, cases):
    table = {}
    for t, expect in (('c', 'active'), ('d', 'active'),
                      ('e', 'evidence_missing'), ('f', 'active')):
        ids = [cid for cid, d in details.items()
               if not d.get('excluded') and cases[cid]['type'] == t]
        ok = [cid for cid in ids if details[cid]['end_state'] == expect]
        flagged = [cid for cid in ids
                   if details[cid]['predicted_positive']]
        table[t] = {'n': len(ids), 'expect_end_state': expect,
                    'matched_expect': ids_ok(ok), 'ok_ids': ok,
                    'flagged': flagged,
                    'deviation_ids': [cid for cid in ids
                                      if details[cid]['end_state'] != expect]}
    return table


def ids_ok(ids):
    return len(ids)


def version_at(repo_dir, sha, rel):
    src = sh(repo_dir, '-c', 'core.autocrlf=false', 'show',
             f'{sha}:{rel}')
    return parse_version(src, sha, rel)


def classify_pair(repo_dir, sha_a, sha_b, rel, local):
    try:
        va = version_at(repo_dir, sha_a, rel)
        vb = version_at(repo_dir, sha_b, rel)
    except Exception as exc:  # git show failure etc.
        return {'category': 'not-found', 'error': str(exc)[:200]}
    fqn = module_fqm(rel) + '.' + local
    ita, itb = va.defs.get(fqn), vb.defs.get(fqn)
    if ita is None or itb is None:
        return {'category': 'not-found'}
    if va.unparsable or vb.unparsable:
        return {'category': 'not-parsable'}
    if ita['ssid'] == itb['ssid'] and ita['span'] == itb['span']:
        return {'category': 'no-change-observed'}
    ta = ast.dump(ita['node'], include_attributes=False)
    tb = ast.dump(itb['node'], include_attributes=False)
    if ta == tb:
        la = ita['span'].split(b'\n')
        lb = itb['span'].split(b'\n')
        diffs = [i for i in range(min(len(la), len(lb))) if la[i] != lb[i]
                 ] + list(range(min(len(la), len(lb)),
                                max(len(la), len(lb))))
        if not diffs:
            return {'category': 'no-change-observed'}
        comment = any(la[i].lstrip().startswith(b'#')
                      for i in diffs if i < len(la)) or \
            any(lb[i].lstrip().startswith(b'#')
                for i in diffs if i < len(lb))
        blank = all((not la[i].strip(b' \t'))
                    for i in diffs if i < len(la)) and \
            all((not lb[i].strip(b' \t')) for i in diffs if i < len(lb))
        if comment:
            return {'category': 'comment-adjacent'}
        if blank:
            return {'category': 'whitespace/blank-line-only'}
        d1 = dump_or(ita, 'args') != dump_or(itb, 'args')
        d2 = dump_or(ita, 'returns') != dump_or(itb, 'returns')
        d3 = dump_or(ita, 'decorator_list') != dump_or(itb, 'decorator_list')
        d4 = docstring_of(ita['node']) != docstring_of(itb['node'])
        if not (d1 or d2 or d3 or d4):
            return {'category': 'whitespace/blank-line-only'}
        best = 'other'
        if d1 or d2:
            best = 'signature-only'
        elif d3:
            best = 'decorator-only'
        elif d4:
            best = 'docstring-only'
        return {'category': best,
                'args': d1, 'returns': d2, 'decorators': d3,
                'docstring': d4}
    parts = {}
    parts['args'] = dump_or(ita, 'args') != dump_or(itb, 'args')
    parts['returns'] = dump_or(ita, 'returns') != dump_or(itb, 'returns')
    parts['annotations'] = ann_dump(ita) != ann_dump(itb)
    parts['decorator_list'] = dump_or(ita, 'decorator_list') != \
        dump_or(itb, 'decorator_list')
    parts['docstring'] = docstring_of(ita['node']) != \
        docstring_of(itb['node'])
    body_diff = ta != tb and not any(parts.values())
    if parts['docstring'] and parts['args']:
        best = 'signature-only'
    elif parts['docstring'] and parts['decorator_list']:
        best = 'decorator-only'
    elif parts['docstring']:
        best = 'docstring-only'
    elif parts['args'] or parts['returns']:
        best = 'signature-only'
    elif parts['decorator_list']:
        best = 'decorator-only'
    elif parts['annotations']:
        best = 'annotation-only'
    elif body_diff:
        best = 'other'
    else:
        best = 'other'
    return {'category': best, 'parts': {k: v for k, v in parts.items()}}


def dump_or(item, field):
    val = getattr(item['node'], field, None)
    if val is None:
        return None
    if isinstance(val, list):
        return '|'.join(ast.dump(x, include_attributes=False)
                        for x in val)
    return ast.dump(val, include_attributes=False)


def ann_dump(item):
    node = item['node']
    args = getattr(node, 'args', None)
    bits = []
    if getattr(node, 'returns', None) is not None:
        bits.append(ast.dump(node.returns, include_attributes=False))
    if args is not None:
        for arg in ast.walk(args):
            ann = getattr(arg, 'annotation', None)
            if ann is not None:
                bits.append(ast.dump(ann, include_attributes=False))
    return ';'.join(bits)


def docstring_of(node):
    if node.body and isinstance(node.body[0], ast.Expr) \
            and isinstance(node.body[0].value, ast.Constant) \
            and isinstance(node.body[0].value.value, str):
        return node.body[0].value.value
    return None


def bisect_check(cases, details, doc, args):
    """FP cause for type b flagged; FN cause for type a not flagged."""
    reasons = {}
    for cid, d in details.items():
        if d.get('excluded'):
            continue
        case = cases[cid]
        if case['type'] == 'b' and d['predicted_positive']:
            first = d['flagged_at']
        elif case['type'] == 'a' and not d['predicted_positive']:
            first = max(d['sha_state']) if d['sha_state'] else None
        else:
            continue
        if first is None or first not in d['sha_state']:
            reasons[cid] = {'category': 'no-change-observed'}
            continue
        repo_dir = os.path.join(args.repos_root, case['repo'])
        with open(os.path.join(args.lineages, case['repo'] + '.json'),
                  encoding='utf-8') as fh:
            lin = json.load(fh)
        res = classify_pair(repo_dir, lin[case['t0']], d['sha_state'][first],
                            case['cite_path'], case['local'])
        reasons[cid] = res
    return reasons


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--cases', required=True)
    ap.add_argument('--steps', required=True)
    ap.add_argument('--cases-rows', required=True)
    ap.add_argument('--repos-root', required=True)
    ap.add_argument('--lineages', required=True)
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    with open(args.cases, encoding='utf-8') as fh:
        doc = json.load(fh)
    cases = {c['case_id']: dict(c) for c in doc['cases']}
    steps = bucket_steps(load_jsonl(args.steps))
    case_rows = load_jsonl(args.cases_rows)
    merge_memory_ids(cases, case_rows)
    add_failures = {r['case_id'] for r in case_rows if 'error' in r}
    details = per_case(steps, cases, add_failures)
    core, a_ids, b_ids = core_metrics(details, cases)
    ci = bootstrap_ci(a_ids, b_ids, details)
    report = {'seed': SEED, 'k': K,
              'pass_rule': core['pass_rule'],
              'core': {k: v for k, v in core.items() if k != 'pass_rule'},
              'bootstrap': ci,
              'specials': specials_table(details, cases),
              'excluded_add_failed': sorted(add_failures),
              'cause_categories': [],
              'details': details}
    report['causes'] = bisect_check(cases, details, doc, args)
    table = {}
    for cid, res in report['causes'].items():
        cat = res.get('category', 'other')
        table.setdefault(cat, []).append(cid)
    report['cause_categories'] = {c: sorted(table[c]) for c in CATEGORY_ORDER
                                  if table.get(c)}
    text = json.dumps(report, indent=1)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)) or '.',
                    exist_ok=True)
        with open(args.out, 'w', encoding='utf-8') as fh:
            fh.write(text)
    print(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
