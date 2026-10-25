#!/usr/bin/env python3
"""Replay runner for the Veylqora memory-staleness study.

Committed together with build_cases.py BEFORE any Veylqora build or run. It
executes, for every replay group in cases.json, the REPLAY MECHANICS rule:

REPLAY MECHANICS (verbatim; fixed before running and restated in scripts/
staleness_replay/README.md and docs/research/staleness-replay-summary.md):

  Find the veylqora binary (arg --vey). Probe it with --help and require the
  subcommands init, index, memory with sub-commands add, list, inspect; if
  the probe fails, stop before touching any repository. Groups come from
  cases.json ("groups": sorted by repo and t0). Each group runs on its own
  fresh database file (--db) under the results directory, on the clone under
  --repos-root/<repo>:

    1. `git -C <repo> checkout -q -f --detach <sha(t0)>`.
    2. `veylqora init --db <db>`.
    3. `veylqora index <repo> --db <db> --json` (full index at t0).
    4. For every case of the group, in cases.json order:
       `veylqora memory add --root <repo> --type decision --content
       "Study case <case_id>: cites <cite_fqn>" --author staleness-replay
       --evidence <cite_path>:<cite_fqn> --db <db> --json`; the returned
       memory id is stored. A failing add is recorded as an error row and
       the case is excluded from later steps (never invented).
    5. `veylqora memory list --root <repo> --db <db> --json` is recorded as
       initial per-case states.
    6. For step s = 1..group horizon: checkout --detach <sha(t0+s)>,
       `veylqora index <repo> --db <db> --json`, then
       `veylqora memory list --json`; each state map (memory id -> state)
       is recorded into steps.jsonl. At steps equal to a case's driver
       commit offset (`driver.commit - case.t0`) one extra
       `veylqora memory inspect <id> --json` per case is recorded.
    7. The database file is deleted after the group; the clone is left
       detached at the group's t0 commit.

  Everything observed is written out: add failures, index errors, parse
  fallbacks. The runner makes no judgement calls and computes no scores.

CLI: python3 replay_staleness.py --cases <cases.json> --repos-root <dir>
     --results <results_dir> --vey <vey_binary> [--pilot] [--groups a,b]
Results: <results_dir>/env.json, /steps.jsonl, /cases.jsonl, /groups.jsonl
(pilot rows carry "pilot": true).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time


def run(cmd, timeout=600):
    t0 = time.monotonic()
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout, check=False)
    return proc.returncode, proc.stdout.decode('utf-8', 'replace'), \
        proc.stderr.decode('utf-8', 'replace'), time.monotonic() - t0


class Appender:
    def __init__(self, path):
        self.path = path
        self.fh = open(path, 'a', encoding='utf-8')

    def row(self, obj):
        self.fh.write(json.dumps(obj) + '\n')
        self.fh.flush()
        os.fsync(self.fh.fileno())


def probe_vey(vey):
    rc, out, err, _ = run([vey, '--help'])
    text = (out + err).lower()
    missing = [s for s in ('init', 'index', 'memory add',
                           'memory list', 'memory inspect') if s not in text]
    if missing:
        raise SystemExit(f'veylqora probe failed: rc={rc} missing={missing}\n'
                         f'{out}\n{err}')
    return True


def parse_states(out):
    """memory list --json -> {id: state}; tolerate list or {items|memories}."""
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return None, f'non-json memory list output: {out[:200]}'
    if isinstance(data, dict) and 'items' in data:
        data = data['items']
    if isinstance(data, dict) and 'memories' in data:
        data = data['memories']
    if isinstance(data, list):
        states = {}
        for item in data:
            if isinstance(item, dict) and 'id' in item and 'state' in item:
                states[item['id']] = item['state']
            else:
                return None, f'unexpected list item: {str(item)[:200]}'
        return states, None
    return None, f'unexpected memory list shape: {str(data)[:200]}'


def json_value(out):
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return None


def replay_group(vey, repo_dir, group, cases, lineage, results, args_vey,
                 app, aps, apc, pilot):
    repo = group['repo']
    t0 = group['t0']
    horizon = group['horizon']
    tag = f'{repo}@{t0}'
    db = os.path.join(results, 'db', f'{repo}-{t0}.db')
    if lineage is None or len(lineage) <= t0 + horizon:
        app.row({'group': tag, 'error': 'missing lineage coverage'})
        return False
    rc, out, err, dt = run(['git', '-C', repo_dir, 'checkout', '-q', '-f',
                            '--detach', lineage[t0]])
    if rc != 0:
        app.row({'group': tag, 'error': f'checkout t0 failed rc={rc}: {err}'})
        return False
    rc, out, err, dt = run([vey, 'init', '--db', db])
    init_row = {'rc': rc, 'dt': round(dt, 3), 'err': err[-400:] if err else ''}
    if rc != 0:
        app.row({'group': tag, 'error': f'init failed: {err}'})
        return False
    rc, out, err, dt = run([vey, 'index', repo_dir, '--db', db, '--json'])
    full_index = {'rc': rc, 'dt': round(dt, 3),
                  'out': out[:2000], 'err': err[-400:] if err else ''}
    if rc != 0:
        app.row({'group': tag, 'error': f'full index failed: {err}'})
        return False
    ids: dict = {}
    errors: dict = {}
    for case in cases:
        rc, out, err, dt = run([vey, 'memory', 'add', '--root', repo_dir,
                                '--type', 'decision',
                                '--content',
                                f'Study case {case["case_id"]}: '
                                f'cites {case["cite_fqn"]}',
                                '--author', 'staleness-replay',
                                '--evidence',
                                f'{case["cite_path"]}:{case["cite_fqn"]}',
                                '--db', db, '--json'])
        value = json_value(out)
        mid = None
        if isinstance(value, dict):
            for k in ('id', 'memory_id', 'memoryId'):
                if k in value:
                    mid = value[k]
                    break
        if rc != 0 or mid is None:
            errors[case['case_id']] = (f'add failed rc={rc} '
                                       f'out={out[:300]} err={err[:300]}')
            continue
        ids[case['case_id']] = mid
    states, serr = parse_states_of(vey, repo_dir, db)
    aps.row({'group': tag, 'repo': repo, 't0': t0, 's': 0,
             'sha': lineage[t0], 'full_index': True, 'init': init_row,
             'add_errors': sorted(errors), 'states': states,
             'list_error': serr, 'dt_index': full_index['dt'],
             'pilot': pilot})
    for case in cases:
        cid = case['case_id']
        base = {'group': tag, 'repo': repo, 'case_id': cid,
                'type': case['type'], 't0': t0,
                'cite_path': case['cite_path'],
                'cite_fqn': case['cite_fqn'], 'memory_id': ids.get(cid),
                'pilot': pilot}
        if cid in errors:
            base['error'] = errors[cid]
        apc.row(base)
    inspect_at: dict = {}
    for case in cases:
        if case['case_id'] in errors or case['case_id'] not in ids:
            continue
        if 'commit' not in case['driver']:
            continue  # b/f anchors have no driver commit: nothing to inspect
        off = case['driver']['commit'] - t0
        if 0 <= off <= horizon:
            inspect_at.setdefault(off, []).append(
                (case['case_id'], ids[case['case_id']]))
    for cid, mid in inspect_at.get(0, []):
        rc, out, err, _ = run([vey, 'memory', 'inspect', mid, '--root',
                               repo_dir, '--db', db, '--json'])
        apc.row({'group': tag, 'repo': repo, 'case_id': cid,
                 'inspect_at': 0, 'rc': rc,
                 'out': out[:4000], 'err': err[:400], 'pilot': pilot})
    for s in range(1, horizon + 1):
        sha = lineage[t0 + s]
        rc, out, err, dt = run(['git', '-C', repo_dir, 'checkout', '-q',
                                '-f', '--detach', sha])
        if rc != 0:
            app.row({'group': tag, 'error': f'checkout s{s} failed: {err}'})
            return False
        rc, out, err, dt = run([vey, 'index', repo_dir, '--db', db,
                                '--json'])
        irow = {'rc': rc, 'dt': round(dt, 3),
                'changed_shas': json_len(out), 'err': err[-300:] if err else ''}
        states, serr = parse_states_of(vey, repo_dir, db)
        aps.row({'group': tag, 'repo': repo, 't0': t0, 's': s, 'sha': sha,
                 'full_index': False, 'index': irow, 'states': states,
                 'list_error': serr, 'pilot': pilot})
        for cid, mid in inspect_at.get(s, []):
            rc2, out2, err2, _ = run([vey, 'memory', 'inspect', mid,
                                      '--root', repo_dir, '--db', db,
                                      '--json'])
            apc.row({'group': tag, 'repo': repo, 'case_id': cid,
                     'inspect_at': s, 'rc': rc2, 'out': out2[:4000],
                     'err': err2[:400], 'pilot': pilot})
    app.row({'group': tag, 'steps_done': horizon,
             'cases_ok': len(ids), 'cases_err': len(errors)})
    try:
        os.remove(db)
    except OSError:
        pass
    return not errors and serr is None


def json_len(out):
    return out.count('"sha"')


def parse_states_of(vey, repo_dir, db):
    rc, out, err, _ = run([vey, 'memory', 'list', '--root', repo_dir,
                           '--db', db, '--json'])
    if rc != 0:
        return None, f'memory list rc={rc}: {err[:300]}'
    return parse_states(out)


def reap_env(vey, repo_dir, results):
    env = {'vey': vey, 'vey_sha': None}
    rc, out, err, _ = run([vey, '--version'])
    env['version'] = (out + err)[:200]
    rc, out, _, _ = run([vey, '--help'])
    env['help_head'] = out[:600]
    os.makedirs(results, exist_ok=True)
    with open(os.path.join(results, 'env.json'), 'w', encoding='utf-8') as fh:
        json.dump(env, fh, indent=1)
    return env


def load_cases(path):
    with open(path, encoding='utf-8') as fh:
        doc = json.load(fh)
    case_map = {c['case_id']: c for c in doc['cases']}
    return doc, case_map


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--cases', required=True)
    ap.add_argument('--repos-root', required=True)
    ap.add_argument('--results', required=True)
    ap.add_argument('--vey', required=True)
    ap.add_argument('--pilot', action='store_true')
    ap.add_argument('--groups', default=None,
                    help='comma list of "repo@t0" to run (default: all)')
    ap.add_argument('--lineages', required=False, default=None,
                    help='dir with <repo>.json files holding lineage shas')
    args = ap.parse_args()
    os.makedirs(os.path.join(args.results, 'db'), exist_ok=True)
    os.makedirs(os.path.join(args.results, 'lineages'), exist_ok=True)
    lineages_dir = args.lineages or os.path.join(args.results, 'lineages')
    doc, case_map = load_cases(args.cases)
    probe_vey(args.vey)
    groups = doc['groups']
    if args.groups:
        wanted = [g.split('@') for g in args.groups.split(',')]
        wanted_pairs = {(r, int(t)) for r, t in wanted}
        groups = [g for g in groups
                  if (g['repo'], g['t0']) in wanted_pairs]
    if args.pilot:
        groups = groups[:3]
    app = Appender(os.path.join(args.results, 'groups.jsonl'))
    aps = Appender(os.path.join(args.results, 'steps.jsonl'))
    apc = Appender(os.path.join(args.results, 'cases.jsonl'))
    reap_env(args.vey, args.repos_root, args.results)
    any_err = False
    for g in groups:
        repo_dir = os.path.join(args.repos_root, g['repo'])
        if not os.path.isdir(repo_dir):
            app.row({'group': f'{g["repo"]}@{g["t0"]}',
                     'error': 'repo dir missing'})
            any_err = True
            continue
        lin_path = os.path.join(lineages_dir, f'{g["repo"]}.json')
        if not os.path.exists(lin_path):
            rc, out, _, _ = run(['git', '-C', repo_dir,
                                 'rev-list', '--first-parent', 'HEAD'])
            shas = [s for s in out.split('\n') if s]
            shas.reverse()
            lineage = shas
            app.row({'group': f'{g["repo"]}@{g["t0"]}',
                     'lineage_written': len(lineage)})
        else:
            with open(lin_path, encoding='utf-8') as fh:
                lineage = json.load(fh)
        with open(lin_path, 'w', encoding='utf-8') as fh:
            json.dump(lineage, fh)
        cases = [case_map[cid] for cid in g['case_ids'] if cid in case_map]
        ok = replay_group(args.vey, repo_dir, g, cases, lineage,
                          args.results, args.vey, app, aps, apc, args.pilot)
        if not ok:
            any_err = True
    app.row({'run_done': True, 'pilot': args.pilot,
             'any_errors': any_err, 'groups': len(groups)})
    return 1 if any_err else 0


if __name__ == '__main__':
    raise SystemExit(main())
