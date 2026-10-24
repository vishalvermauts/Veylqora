#!/usr/bin/env python3
"""Case builder for the Veylqora memory-staleness replay.

Written and committed BEFORE any Veylqora build or run, right after
label_history.py. It reads only the labels produced by label_history.py and
selects replay cases. It knows nothing about veylqora's behaviour; it only
encodes this rule:

CASE RULE (verbatim; fixed before running and restated in scripts/
staleness_replay/README.md and docs/research/staleness-replay-summary.md):

  All facts come from labels.json files produced by label_history.py (LABEL
  RULE in its docstring). Event positions i are indices into the repo's
  first-parent lineage (oldest = 0). For one case the replay creates a
  veylqora memory that cites one definition - (cite_path, cite_fqn) - at
  commit t0, then re-indexes every commit t0+1 .. horizon (window cap
  K = 20 commits after t0) and records veylqora's memory states per step.
  Dedup key for every case: (repo, cite_path, cite_fqn, t0); a case is
  never built twice for the same key. The validity filter for special
  events uses the `matches` field of the LABEL RULE: renamed valid iff
  matches == 1, moved valid iff matches == 1, deleted valid iff
  matches == 0; excluded-with-note counts are reported, never silently
  dropped. Type passes run in the fixed order (a), then (b), then specials
  in the order renamed, moved, deleted, callee_only.

    a body_change        t0 = c-1 where c is the commit of a body_changed
                         event (moved flag false); cite_fqn = the event's
                         fqn, cite_path = the event's path; horizon = the
                         step at which c occurs; expect flagged
                         (suspected_stale at or before step c-t0).
    b unchanged_neighbours_change
                         anchor at a quiet gap of one chain (no event of
                         the chain between two consecutive events);
                         t0 = m1-1 where m1 is the first file-change bound
                         of the same file identity inside the gap; the
                         window must contain at least one more file-change
                         bound m2 <= t0+K; horizon = min(K, m2-t0)
                         (deviation fix 2026-10-24: first coded as
                         min(t0+K, m2) - an absolute index - while every
                         other case type stores step offsets and replay
                         and scorer consume offsets; case selection
                         unaffected; see the summary doc);
                         expect never flagged (the cited definition's
                         span bytes do not change in the window).
    c renamed            name_changed event with matches == 1; cite_fqn =
                         event old_fqn, cite_path = event old_path;
                         t0 = c-1; horizon = c; expect re-anchored and
                         still active (end state active, never flagged).
                         The renamed_away partner event of the same pair
                         (matches == 1) yields the symmetric case citing
                         the old name.
    d moved              moved event with matches == 1; cite_fqn = event
                         old_fqn, cite_path = event old_path; t0 = c-1;
                         horizon = c; expect re-anchored and still active
                         (end state active, never flagged).
    e deleted            deleted event with matches == 0; cite_fqn and
                         cite_path straight from the event (it carries the
                         removed name and its path); t0 = c-1; horizon = c;
                         expect evidence_missing (flagged).
    f callee_only_changed
                         anchor at a chain whose callees include the local
                         name of a same-file sibling chain that has a
                         body_changed event at commit c2 inside the
                         anchor's quiet gap; t0 = max(prev event of the
                         anchor, c2-5) and < c2; horizon = c2; expect
                         never flagged (only the callee's body changes).

    Relaxation for (b) when candidates are scarce, level 0 first then 1
    then 2: level 0 requires the quiet gap to span at least 3 commits and
    at least 2 file-change bounds in the window; level 1 requires a gap of
    at least 2 commits (still 2 bounds); level 2 requires a gap of at
    least 1 commit and only 1 bound. Each case records its relax level;
    relaxation ledger is written to the output.

    Caps and quotas: per repo the (a) pass may take at most 10 candidates,
    then (b) fills that repo's core pool up to 20 core cases (a+b;
    hard floor 15 per repo - shortfalls are reported, never invented).
    Anchors are stratified by lineage thirds (0..n/3..2n/3..n) picked
    round-robin, each slot sorted by (event commit, identity, chain).
    Candidates whose t0 belongs to a replay group that already exists for
    the repo are taken greedily before candidates at fresh t0 values, to
    limit the number of full indexes. Specials are global across repos
    with quotas: >=5 renamed, >=3 moved, >=8 deleted, >=8 callee_only;
    specials are taken round-robin across repos.

  Seed 20261024 fixes tie-breaking randomness (currently none: all
  orderings are total or lexicographic; the seed is recorded anyway).

CLI: python3 build_cases.py --labels <labels.json> [<labels2.json> ...]
     --out <cases.json>

Output JSON: {"seed", "k", "repos": {name: stats}, "relaxations": [...],
 "excluded": {...}, "counts": {repo: {"a", "b", "core", "shortfall"},
 "_specials": {renamed, moved, deleted, callee_only}}, "groups":
 [{"repo", "t0", "horizon", "case_ids"}], "cases": [{case_id, repo, type,
 name, fid, cid, local, t0, horizon, cite_path, cite_fqn, expect_flagged,
 expect_state, driver: {...}, relax_level?}]}
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from label_history import module_fqm  # noqa: E402

SEED = 20261024
K = 20
A_CAP = 10
CORE_TARGET = 20
CORE_FLOOR = 15
SPECIAL_QUOTAS = {'renamed': 5, 'moved': 3, 'deleted': 8, 'callee_only': 8}
EXPECT_STATE = {
    'a': 'flagged', 'b': 'active', 'c': 'active', 'd': 'active',
    'e': 'evidence_missing', 'f': 'active',
}
TMIR = {'a': 'body_change', 'b': 'unchanged_neighbours_change',
        'c': 'renamed', 'd': 'moved', 'e': 'deleted',
        'f': 'callee_only_changed'}


def load_labels(paths):
    repos = {}
    for path in paths:
        with open(path, encoding='utf-8') as fh:
            data = json.load(fh)
        (name, payload), = data.items()
        repos[name] = payload
    return repos


def repo_view(payload):
    """Flatten labels into selection helpers for one repo."""
    v = {'n': len(payload['lineage']), 'chains': [], 'bounds_by_fid': {}}
    for ident in payload['identities']:
        if ident.get('ghost'):
            continue
        fid = ident['id']
        v['bounds_by_fid'][fid] = sorted({b['i'] for b in
                                          ident.get('bounds', [])})
        for ch in ident['chains']:
            if ch['events']:
                v['chains'].append({'fid': fid, 'cid': ch['cid'],
                                    'local': ch['local'],
                                    'events': ch['events']})
    v['chains'].sort(key=lambda c: (c['fid'], c['cid']))
    return v


def slot_of(i, n):
    """0, 1, 2 - lineage thirds for stratified round-robin."""
    return 0 if i < n / 3 else (1 if i < 2 * n / 3 else 2)


def note(ex, key):
    ex[key] = ex.get(key, 0) + 1


def candidate(repo, ttype, fid, cid, local, cand):
    cand['repo'] = repo
    cand['type'] = ttype
    cand['fid'] = fid
    cand['cid'] = cid
    cand['local'] = local
    cand['repo_key'] = (repo, cand['cite_path'], cand['cite_fqn'], cand['t0'])
    return cand


def candidates_type_a(repo, v, ex):
    out = []
    for c in v['chains']:
        evs = c['events']
        for idx, ev in enumerate(evs):
            if ev['kind'] != 'body_changed' or ev.get('moved'):
                continue
            if idx == 0:
                note(ex, 'body_change_no_prior_state')
                continue
            t0 = ev['i'] - 1
            out.append(candidate(repo, 'a', c['fid'], c['cid'], c['local'], {
                't0': t0, 'horizon': ev['i'] - t0,
                'cite_path': ev['path'], 'cite_fqn': ev['fqn'],
                'driver': {'commit': ev['i'], 'kind': 'body_changed',
                           'ssid': ev['ssid']}}))
    return out


def candidates_type_b(repo, v, ex, level):
    min_gap = 3 - level
    min_bounds = 1 if level == 2 else 2
    out = []
    for c in v['chains']:
        evs = c['events']
        prev = evs[0]['i']
        gaps = []
        for ev in evs[1:]:
            gaps.append((prev, ev['i']))
            prev = ev['i']
        gaps.append((prev, v['n'] - 1))
        for lo, hi in gaps:
            if hi - lo < min_gap:
                continue
            file_bounds = [i for i in v['bounds_by_fid'][c['fid']]
                           if lo < i <= hi]
            if len(file_bounds) < min_bounds:
                continue
            m1 = file_bounds[0]
            m2 = file_bounds[1] if len(file_bounds) > 1 else m1
            t0 = m1 - 1
            if t0 < 0 or t0 < lo or m2 > t0 + K:
                note(ex, 'b_window_over_cap')
                continue
            horizon = min(K, m2 - t0)
            at_t0 = [e for e in evs if e['i'] <= t0]
            path_at_t0 = at_t0[-1]['path'] if at_t0 else evs[0]['path']
            fqn = module_fqm(path_at_t0) + '.' + c['local']
            out.append(candidate(repo, 'b', c['fid'], c['cid'], c['local'], {
                't0': t0, 'horizon': horizon,
                'cite_path': path_at_t0, 'cite_fqn': fqn,
                'driver': {'gap': [lo, hi], 'file_bounds': file_bounds[:3]},
                'relax_level': level}))
    return out


def candidates_renamed(repo, v, ex):
    out = []
    for c in v['chains']:
        for ev in c['events']:
            if ev['kind'] not in ('name_changed', 'renamed_away'):
                continue
            if ev.get('matches') != 1:
                note(ex, f"{ev['kind']}_invalid_matches")
                continue
            t0 = ev['i'] - 1
            if ev['kind'] == 'name_changed':
                cite_path, cite_fqn = ev['old_path'], ev['old_fqn']
            else:
                cite_path, cite_fqn = ev['path'], ev['fqn']
            out.append(candidate(repo, 'c', c['fid'], c['cid'], c['local'], {
                't0': t0, 'horizon': ev['i'] - t0,
                'cite_path': cite_path, 'cite_fqn': cite_fqn,
                'driver': {'commit': ev['i'], 'kind': ev['kind'],
                           'ssid': ev['ssid'], 'matches': ev['matches']}}))
    return out


def candidates_moved(repo, v, ex):
    out = []
    for c in v['chains']:
        for ev in c['events']:
            if ev['kind'] != 'moved':
                continue
            if ev.get('matches') != 1:
                note(ex, 'moved_invalid_matches')
                continue
            t0 = ev['i'] - 1
            out.append(candidate(repo, 'd', c['fid'], c['cid'], c['local'], {
                't0': t0, 'horizon': ev['i'] - t0,
                'cite_path': ev['old_path'], 'cite_fqn': ev['old_fqn'],
                'driver': {'commit': ev['i'], 'kind': 'moved',
                           'ssid': ev['ssid'], 'matches': ev['matches']}}))
    return out


def candidates_deleted(repo, v, ex):
    out = []
    for c in v['chains']:
        for ev in c['events']:
            if ev['kind'] != 'deleted':
                continue
            if ev.get('matches') != 0:
                note(ex, 'deleted_invalid_matches')
                continue
            t0 = ev['i'] - 1
            out.append(candidate(repo, 'e', c['fid'], c['cid'], c['local'], {
                't0': t0, 'horizon': ev['i'] - t0,
                'cite_path': ev['path'], 'cite_fqn': ev['fqn'],
                'driver': {'commit': ev['i'], 'kind': 'deleted',
                           'ssid': ev['ssid'], 'matches': ev['matches']}}))
    return out


def candidates_callee(repo, v, ex):
    out = []
    for c in v['chains']:
        evs = c['events']
        cal = list(evs[0].get('callees') or [])
        prev = evs[0]['i']
        for ev in evs[1:] + [None]:
            nxt_i = ev['i'] if ev is not None else v['n'] - 1
            anchor = next(e for e in evs if e['i'] == prev)
            for name in cal:
                sib = next((s for s in v['chains']
                            if s['fid'] == c['fid'] and s['cid'] != c['cid']
                            and s['local'].rsplit('.', 1)[-1] == name), None)
                if sib is None:
                    continue
                for s_ev in sib['events']:
                    if s_ev['kind'] != 'body_changed':
                        continue
                    c2 = s_ev['i']
                    if not (prev < c2 <= nxt_i):
                        continue
                    t0 = max(prev, c2 - 5)
                    if t0 >= c2 or c2 - t0 > K:
                        note(ex, 'callee_over_cap')
                        continue
                    path_at_t0 = anchor['path']
                    fqn = module_fqm(path_at_t0) + '.' + c['local']
                    out.append(candidate(repo, 'f', c['fid'], c['cid'],
                                         c['local'], {
                        't0': t0, 'horizon': c2 - t0,
                        'cite_path': path_at_t0, 'cite_fqn': fqn,
                        'driver': {'commit': c2, 'kind': 'callee_change',
                                   'callee': name,
                                   'callee_ssid': s_ev['ssid']}}))
            if ev is not None:
                if ev.get('callees'):
                    cal = list(ev['callees'])
                prev = ev['i']
    return out


def strat_order(cands, n):
    """Round-robin across lineage thirds; slots keep natural order."""
    by_slot = {0: [], 1: [], 2: []}
    for cand in sorted(cands, key=lambda x: (x['driver'].get('commit',
                                                            x['t0'] + 1),
                                             x['fid'], x['cid'], x['t0'])):
        by_slot[slot_of(cand['driver'].get('commit', cand['t0'] + 1), n)] \
            .append(cand)
    out = []
    turn = 0
    while any(by_slot[s] for s in (0, 1, 2)):
        s = turn % 3
        if by_slot[s]:
            out.append(by_slot[s].pop(0))
        turn += 1
    return out


def take(bucket, cap, used_keys, got):
    """Append up to cap un-used candidates; returns True if any taken."""
    taken = False
    for cand in bucket:
        if len(got) >= cap:
            break
        if cand['repo_key'] in used_keys:
            continue
        got.append(cand)
        used_keys.add(cand['repo_key'])
        taken = True
    return taken


def build(paths, relax_log, excluded):
    repos = load_labels(paths)
    views = {name: repo_view(p) for name, p in repos.items()}
    stats = {}
    for name, v in views.items():
        stats[name] = {'lineage_len': v['n'], 'chains': len(v['chains']),
                       'events': sum(len(c['events']) for c in v['chains'])}
    used_global: set = set()
    t0s_by_repo: dict = {}
    cases: list = []
    counts = {}

    def add_case(cand):
        case = {'case_id': f'{cand["repo"]}-{len(cases):04d}',
                'repo': cand['repo'], 'type': cand['type'],
                'name': TMIR[cand['type']], 'fid': cand['fid'],
                'cid': cand['cid'], 'local': cand['local'],
                't0': cand['t0'], 'horizon': cand['horizon'],
                'cite_path': cand['cite_path'], 'cite_fqn': cand['cite_fqn'],
                'expect_flagged': cand['type'] in ('a', 'e'),
                'expect_state': EXPECT_STATE[cand['type']],
                'driver': cand['driver']}
        if 'relax_level' in cand:
            case['relax_level'] = cand['relax_level']
        cases.append(case)
        t0s_by_repo.setdefault(cand['repo'], set()).add(cand['t0'])

    for repo in sorted(views):
        v = views[repo]
        keys: set = set()
        got_a: list = []
        take(candidates_type_a(repo, v, excluded), A_CAP, keys, got_a)
        for cand in got_a:
            add_case(cand)
        used_global.update(keys)
        got_b: list = []
        remaining = CORE_TARGET - len(got_a)
        for level in (0, 1, 2):
            if remaining <= 0:
                break
            fresh0: list = []
            pool = candidates_type_b(repo, v, excluded, level)
            free = [c for c in pool
                    if c['t0'] in t0s_by_repo.get(repo, set())]
            free.sort(key=lambda x: (x['driver']['gap'][0], x['fid'],
                                     x['cid']))
            if take(free, remaining, keys, fresh0):
                relax_log.append({'repo': repo, 'pass': 'b', 'level': level,
                                  'picked_free': len(fresh0)})
            pool = [c for c in pool if c['repo_key'] not in keys]
            if len(fresh0) < remaining:
                if take(strat_order(pool, v['n']), remaining, keys, fresh0):
                    relax_log.append({'repo': repo, 'pass': 'b',
                                      'level': level,
                                      'picked_strat': len(fresh0)})
            if fresh0:
                for cand in fresh0:
                    got_b.append(cand)
                    add_case(cand)
                remaining -= len(fresh0)
        used_global.update(keys)
        counts[repo] = {'a': len(got_a), 'b': len(got_b),
                        'core': len(got_a) + len(got_b),
                        'shortfall': max(0, CORE_FLOOR
                                         - len(got_a) - len(got_b))}

    builders = {'renamed': candidates_renamed, 'moved': candidates_moved,
                'deleted': candidates_deleted, 'callee_only':
                candidates_callee}
    counts['_specials'] = {}
    for ttype, quota in SPECIAL_QUOTAS.items():
        pools = {repo: builders[ttype](repo, views[repo], excluded)
                 for repo in sorted(views)}
        got = 0
        for repo in sorted(views):
            while got >= quota:
                break
            bucket = [c for c in pools[repo]
                      if c['repo_key'] not in used_global]
            free = [c for c in bucket
                    if c['t0'] in t0s_by_repo.get(repo, set())]
            got0: list = []
            take(free + strat_order(
                [c for c in bucket if c not in free], views[repo]['n']),
                quota - got, used_global, got0)
            for cand in got0:
                add_case(cand)
                got += 1
        counts['_specials'][ttype] = got

    return stats, cases, counts


def build_groups(cases):
    gmap = {}
    for c in cases:
        key = (c['repo'], c['t0'])
        g = gmap.setdefault(key, {'repo': c['repo'], 't0': c['t0'],
                                  'horizon': 0, 'case_ids': []})
        g['case_ids'].append(c['case_id'])
        g['horizon'] = max(g['horizon'], c['horizon'])
    return [gmap[k] for k in sorted(gmap)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--labels', nargs='+', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    relax_log: list = []
    excluded: dict = {}
    stats, cases, counts = build(args.labels, relax_log, excluded)
    doc = {'seed': SEED, 'k': K, 'repos': stats,
           'relaxations': relax_log, 'excluded': excluded,
           'counts': counts, 'groups': build_groups(cases), 'cases': cases}
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or '.', exist_ok=True)
    with open(args.out, 'w', encoding='utf-8') as fh:
        json.dump(doc, fh, indent=1)
    n_types = {t: sum(1 for c in cases if c['type'] == t) for t in TMIR}
    print(f'wrote {args.out}: cases={len(cases)} types={n_types}',
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
