#!/usr/bin/env python3
"""Independent history labeller for the Veylqora memory-staleness replay.

This script is written and committed BEFORE any Veylqora build or run. It uses
only the Python standard library and git. It imports nothing from veylqora and
re-derives everything from the fossil history of the subject repos. CPU-only:
no model, gateway or agent calls anywhere in this study.

LABEL RULE (verbatim; fixed before running and restated in scripts/
staleness_replay/README.md and docs/research/staleness-replay-summary.md):

  Walk the repo's first-parent lineage oldest -> newest
  (`git rev-list --first-parent HEAD`, reversed). For every .py path identity
  that exists at HEAD, reconstruct its change stream along the lineage from one
  streamed `git log --first-parent --name-status` pass over the whole repo,
  run with `-c diff.renames=copies`: `A` means the path is born at that commit,
  `M` means the file version changes while the path stays, `D` ends a lifetime
  (only ever reached for lifetimes that do not contain HEAD), and `R`/`C`
  entries (status old -> new, e.g. click's switch to the src/ layout) carry the
  identity across a move or copy of its file: score 100 carries identical bytes
  (no re-parse), score < 100 carries a modified version. For every
  (commit c, parent p) boundary of the identity, parse both file versions with
  `ast` and catalogue every ast.FunctionDef / ast.AsyncFunctionDef /
  ast.ClassDef anywhere in the module (nested and async definitions included)
  by its module-qualified name `module.Class.method`, where module = rel path
  with slashes replaced by dots and ".py" stripped ("/__init__.py" collapsed,
  the same convention the subject indexer uses). The canonical body of a
  definition is `ast.dump(node, include_attributes=False)` with the
  definition's own `name` replaced by the literal "<fn>", so a rename alone is
  not a body change; nothing else is removed: comments and blank lines are
  inherently absent from ASTs while decorators, docstrings, signatures,
  defaults, annotations and nested statements are all kept. A definition's
  label on a boundary is:

    added          present at c, absent at p
    deleted        present at p, absent at c
    body_changed   present at both, canonical bodies differ
    name_changed   present at c under a new name whose canonical body equals
                   the canonical body of a definition present at p that is
                   absent at c (matched within the same file identity; the
                   partner definition keeps a `renamed_away` event)
    moved          present at both, canonical body identical and local name
                   identical, while the file identity's path changed on this
                   boundary
    renamed_away   the name-side partner of a name_changed event
    unchanged      present at both, canonical bodies identical (no event)

  A body_change that coincides with the file identity changing path carries
  `moved: true`. Special events carry `matches`: the number of live
  definitions in the whole tree at that commit whose masked span bytes share
  the same sha256 (own location included; matches == 1 for a rename/move means
  the renamed or moved definition is the only live definition with that
  fingerprint, matches == 0 for a deletion means no other definition in the
  tree can keep the reference alive). Callees of a definition are the names of
  `ast.Call` targets inside it - bare `Name` targets and attribute names of
  `Name.attr` / `self.method` targets - intersected with the last naming
  segments of the definitions found in the same file version. Unparsable or
  non-UTF-8 file versions are marked "unparsable" (no def labels are produced
  across them; the last parsable version stays in force) and a
  module-qualified name that is mapped twice in one file version produces a
  "dup_shadow" marker.

  Bookkeeping scope for `matches`: the whole tree includes dead file
  lifetimes (paths whose name-status stream ends in a `D` before HEAD, and
  earlier lifetimes of HEAD paths). Those lifetimes are tracked silently as
  "ghost" identities so their definitions enter and leave `matches` exactly
  when their file enters and leaves the tree, but ghosts produce no chains
  and no events in the output.

CLI: python3 label_history.py <repo_dir> <out_labels.json>

Output JSON: {"<repo>": {"lineage": [<sha>, oldest..newest],
 "identities": [{"id", "path", "ghost", "markers": [...], "chains": [
   {"cid", "fid", "local", "renamed_from", "old_fqn", "old_prev_i", "events":
    [{"i", "kind", "path", "fqn", "old_fqn"?, "callees", "ssid", "prev_i",
      "matches"?}]}]}]}}
Ghost identities carry "ghost": true and always have empty "chains" and
"markers"; they exist only to keep the `matches` counts faithful to the rule
above.
"""

from __future__ import annotations

import ast
import hashlib
import json
import os
import re
import subprocess
import sys


def sh(repo: str, *args: str) -> bytes:
    proc = subprocess.run(
        ['git', '-C', repo, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return proc.stdout


def module_fqm(rel_path: str) -> str:
    """rel path -> module name (same convention the subject indexer uses)."""
    if rel_path == '__init__.py':
        return 'root'
    if rel_path.endswith('/__init__.py'):
        return rel_path[: -len('/__init__.py')].replace('/', '.')
    return rel_path[:-3].replace('/', '.')


def class_local(prev: str | None, name: str) -> str:
    """the local suffix of a def inside a file version (Class.method chains)."""
    return f'{prev}.{name}' if prev else name


class BlobSource:
    """Persistent `git cat-file --batch` reader."""

    def __init__(self, repo: str):
        self.proc = subprocess.Popen(
            ['git', '-C', repo, 'cat-file', '--batch'],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )
        assert self.proc.stdin is not None and self.proc.stdout is not None

    def bytes_at(self, sha: str, rel: str) -> bytes | None:
        try:
            self.proc.stdin.write(f'{sha}:{rel}\n'.encode())
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            return None
        header = self.proc.stdout.readline()
        if not header:
            return None
        parts = header.decode('latin1').strip().split()
        if not parts or parts[-1] == 'missing':
            return None
        size = int(parts[-1])
        payload = self.proc.stdout.read(size)
        self.proc.stdout.read(1)
        return payload


def dump_masked(node: ast.AST) -> str:
    dump = ast.dump(node, include_attributes=False)
    own = getattr(node, 'name', None)
    if own is not None:
        dump = dump.replace(f'name={own!r},', 'name=<fn>,', 1)
    return dump


class Version:
    """One parsed file version: defs keyed by module-qualified fqn."""

    __slots__ = ('sha', 'rel', 'defs', 'dups', 'unparsable')

    def __init__(self, sha: str, rel: str, defs: dict, dups: set, unparsable: str):
        self.sha = sha
        self.rel = rel
        self.defs = defs
        self.dups = dups
        self.unparsable = unparsable


def call_names(node: ast.AST) -> set:
    names: set = set()
    for sub in ast.walk(node):
        if not isinstance(sub, ast.Call):
            continue
        target = sub.func
        if isinstance(target, ast.Name):
            names.add(target.id)
        elif isinstance(target, ast.Attribute):
            names.add(target.attr)
    return names


def parse_version(src: bytes, sha: str, rel: str) -> Version:
    """One ast pass: defs by fqn with node, masked-span ssid and callees."""
    defs: dict = {}
    dups: set = set()
    try:
        text = src.decode('utf-8')
        tree = ast.parse(text)
    except (UnicodeDecodeError, SyntaxError, ValueError) as exc:
        return Version(sha, rel, defs, dups, f'{type(exc).__name__}: {exc}')

    rows: list = [0]
    pos = 0
    while True:
        nl = src.find(b'\n', pos)
        if nl == -1:
            break
        pos = nl + 1
        rows.append(pos)
    prefix = module_fqm(rel)

    def walk(current_local: str, container) -> None:
        for node in list(ast.iter_child_nodes(container)):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                local = class_local(current_local, node.name)
                fqn = f'{prefix}.{local}'
                start = min(
                    [(d.lineno, d.col_offset) for d in node.decorator_list]
                    + [(node.lineno, node.col_offset)]
                )
                span = src[rows[start[0] - 1] + start[1]:
                           rows[node.end_lineno - 1] + node.end_col_offset]
                span = span.replace(b'\r\n', b'\n').replace(b'\r', b'\n')
                kw = b'class' if isinstance(node, ast.ClassDef) else b'def'
                pattern = re.compile(
                    kw + rb'[ \t]+' + re.escape(node.name.encode('utf-8'))
                    + rb'(?![A-Za-z0-9_$])'
                )
                masked = pattern.sub(b'', span, count=1)
                if fqn in defs:
                    dups.add(fqn)
                else:
                    defs[fqn] = {'node': node, 'local': local,
                                 'ssid': hashlib.sha256(masked).hexdigest(),
                                 'span': span}
                walk(local, node)
            else:
                walk(current_local, node)

    walk('', tree)
    last_names = frozenset(fqn.rsplit('.', 1)[-1] for fqn in defs)
    for fqn, item in defs.items():
        item['callees'] = sorted(call_names(item['node']) & last_names)
    return Version(sha, rel, defs, dups, '')


def stream_log(repo: str):
    """One streaming pass; returns (lineage, touches)."""
    lineage = sh(repo, 'rev-list', '--first-parent', 'HEAD').decode().split('\n')
    lineage = [sha for sha in lineage if sha]
    lineage.reverse()
    chron = {sha: i for i, sha in enumerate(lineage)}

    touches: dict = {}
    proc = subprocess.Popen(
        ['git', '-C', repo, '-c', 'diff.renames=copies',
         'log', '--first-parent', '--name-status', '--format=%x01%H %P', 'HEAD'],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=1 << 20,
    )
    rec_commit = rec_parent = None
    assert proc.stdout is not None
    for raw in proc.stdout:
        text = raw.decode('utf-8', errors='replace')
        if text.startswith('\x01'):
            rec_commit, _, parents = text[1:].strip().partition(' ')
            rec_parent = parents.split(' ')[0] if parents else ''
            idx = chron.get(rec_commit)
            if idx is None:
                rec_commit = rec_parent = None
            elif idx > 0:
                assert rec_parent == lineage[idx - 1], \
                    f'first-parent mismatch at {rec_commit}'
            continue
        if rec_commit is None:
            continue
        parts = text.rstrip('\n').split('\t')
        if not parts or not parts[0]:
            continue
        status = parts[0]
        p1 = parts[1] if len(parts) > 1 else ''
        p2 = parts[2] if len(parts) > 2 else ''
        if not (p1.endswith('.py') or p2.endswith('.py')):
            continue
        idx = chron[rec_commit]
        code = status[0]
        score = 0
        if code in ('R', 'C') and len(status) > 1:
            digits = ''.join(c for c in status[1:] if c.isdigit())
            score = int(digits) if digits else 0
        if code == 'R' and p1 and p2:
            touches.setdefault(p2, []).append((idx, 'R_new', (p1, score)))
            touches.setdefault(p1, []).append((idx, 'R_old', (p2, score)))
        elif code == 'C' and p1 and p2:
            touches.setdefault(p2, []).append((idx, 'A', None))
        elif code == 'A' and p1:
            touches.setdefault(p1, []).append((idx, 'A', None))
        elif code == 'D' and p1:
            touches.setdefault(p1, []).append((idx, 'D', None))
        else:
            touches.setdefault(p1, []).append((idx, 'M', None))
    proc.wait()
    for lst in touches.values():
        lst.sort()
    return lineage, touches


def boundary_chase(path: str, touches: dict, tip: int) -> list:
    """Walk one path history backwards to birth; emit boundary records ascending.

    `tip` is the newest index the identity exists at (HEAD index for tracked
    identities, death-1 for ghosts). Invariant: the identity exists at chron
    `cur` (initially `tip`) under `name`. Each touch interpretation preserves
    the invariant. A D or R_old touch reached here belongs to a PREVIOUS
    lifetime of the same name (the walking lifetime always ends at its own A
    boundary or at the root commit), so the walk stops.
    """
    name = path
    cur = tip
    out: list = []
    while True:
        cands = [t for t in touches.get(name, []) if t[0] <= cur]
        if not cands:
            out.append({'i': 0, 'kind': 'ROOT', 'path_c': name, 'path_p': name,
                        'score': 0})
            break
        idx, kind, payload = max(cands)
        if kind == 'M':
            out.append({'i': idx, 'kind': 'MOD', 'path_c': name, 'path_p': name,
                        'score': 0})
            cur = idx - 1
        elif kind == 'A':
            out.append({'i': idx, 'kind': 'CREATE', 'path_c': name, 'path_p': name,
                        'score': 0})
            break
        elif kind == 'D':
            out.append({'i': max(idx - 1, 0), 'kind': 'ROOT', 'path_c': name,
                        'path_p': name, 'score': 0})
            break
        elif kind == 'R_new':
            old, score = payload
            out.append({'i': idx, 'kind': 'MOVE', 'path_c': name, 'path_p': old,
                        'score': score})
            cur = idx - 1
            name = old
        else:
            out.append({'i': max(idx - 1, 0), 'kind': 'ROOT', 'path_c': name,
                        'path_p': name, 'score': 0})
            break
        if cur < 0:
            out.append({'i': 0, 'kind': 'ROOT', 'path_c': name, 'path_p': name,
                        'score': 0})
            break
    out.reverse()
    return out


def label_repo(repo_dir: str, repo_name: str):
    lineage, touches = stream_log(repo_dir)
    n = len(lineage)
    tracked = [
        p.decode('utf-8', errors='replace')
        for p in sh(repo_dir, 'ls-files', '-z', '--', '*.py').split(b'\x00') if p
    ]
    tracked.sort()
    n_ghosts = sum(1 for lst in touches.values() for t in lst if t[1] == 'D')
    print(f'[{repo_name}] lineage={n} tracked_py={len(tracked)} '
          f'ghost_lifetimes={n_ghosts}', file=sys.stderr)

    identities: list = []
    for ordinal, path in enumerate(tracked):
        bounds = boundary_chase(path, touches, n - 1)
        identities.append({
            'id': ordinal,
            'path': path,
            'ghost': False,
            'bounds': bounds,
            'bounds_summary': [{'i': b['i'], 'kind': b['kind']} for b in bounds],
            'markers': [],
        })
    tracked_count = len(identities)

    # Ghost identities: one per D touch (dead file lifetime). Bookkeeping only.
    deaths: list = []
    for gpath, lst in touches.items():
        for (idx, kind, payload) in lst:
            if kind == 'D':
                deaths.append((idx, gpath))
    deaths.sort()
    for d_idx, gpath in deaths:
        gfid = len(identities)
        identities.append({
            'id': gfid,
            'path': gpath,
            'ghost': True,
            'bounds': boundary_chase(gpath, touches, max(d_idx - 1, 0))
            + [{'i': d_idx, 'kind': 'DEATH', 'path_c': gpath, 'path_p': gpath,
                'score': 0}],
            'bounds_summary': [],
            'markers': [],
        })

    at: list = [[] for _ in range(n)]
    for fid, identity in enumerate(identities):
        for bound in identity['bounds']:
            at[bound['i']].append((fid, bound))

    blobs = BlobSource(repo_dir)

    version_cache: dict = {}

    def version_at(i: int, rel: str) -> Version | None:
        key = (lineage[i], rel)
        if key in version_cache:
            return version_cache[key]
        src = blobs.bytes_at(lineage[i], rel)
        ver = parse_version(src, lineage[i], rel) if src is not None else None
        if len(version_cache) > 4096:
            version_cache.clear()
        version_cache[key] = ver
        return ver

    chains: list = []
    chain_by_cid: dict = {}         # cid -> chain dict
    chain_of_local: dict = {}       # (fid, local) -> cid, for LIVING chains
    live: list = [None] * len(identities)   # last parsable Version per identity
    live_index: dict = {}           # ssid -> number of live defs holding it
    pending: list = []              # special events of the epoch, awaiting matches

    def bump(ssid: str, delta: int) -> None:
        nxt = live_index.get(ssid, 0) + delta
        if nxt:
            live_index[ssid] = nxt
        else:
            live_index.pop(ssid, None)

    def new_chain(fid: int, local: str, renamed_from: str | None,
                  old_fqn: str | None, old_prev_i: int | None) -> str:
        cid = f'{fid}.{len(chains)}'
        chains.append({
            'cid': cid,
            'fid': fid,
            'local': local,
            'renamed_from': renamed_from,
            'old_fqn': old_fqn,
            'old_prev_i': old_prev_i,
            'events': [],
        })
        chain_by_cid[cid] = chains[-1]
        chain_of_local[(fid, local)] = cid
        return cid

    def push(ev: dict, cid: str) -> None:
        chain = chain_by_cid[cid]
        ev['prev_i'] = chain['events'][-1]['i'] if chain['events'] else ev['i']
        chain['events'].append(ev)

    def apply_boundary(fid: int, bound: dict, i: int) -> None:
        kind = bound['kind']
        rel_c = bound['path_c']
        rel_p = bound['path_p']
        ev_ok = not identities[fid]['ghost']

        if kind == 'DEATH':
            ver = live[fid]
            if ver is not None:
                for item in ver.defs.values():
                    bump(item['ssid'], -1)
            live[fid] = None
            return

        if kind == 'MOVE' and bound['score'] == 100:
            ver = live[fid]
            if ver is not None:
                mod_old = module_fqm(rel_p)
                mod_new = module_fqm(rel_c)
                if ev_ok:
                    for item in sorted(ver.defs.values(),
                                       key=lambda d: d['local']):
                        local = item['local']
                        cid = chain_of_local.get((fid, local))
                        if cid is None:
                            continue  # shadowed defs have no chain
                        ev = {'i': i, 'kind': 'moved',
                              'fqn': mod_new + '.' + local,
                              'old_fqn': mod_old + '.' + local,
                              'callees': item['callees'], 'ssid': item['ssid'],
                              'path': rel_c, 'old_path': rel_p}
                        push(ev, cid)
                        pending.append(ev)
                rekeyed = {mod_new + '.' + it['local']: it
                           for it in ver.defs.values()}
                ndups = {mod_new + '.' + ver.defs[f]['local']
                         for f in ver.dups if f in ver.defs}
                live[fid] = Version(ver.sha, rel_c, rekeyed, ndups,
                                    ver.unparsable)
            return

        child = version_at(i, rel_c)
        if child is None or child.unparsable:
            if ev_ok:
                identities[fid]['markers'].append({'i': i, 'kind': 'unparsable'})
            return  # the last parsable version stays in force

        parent = live[fid]
        is_birth = kind in ('CREATE', 'ROOT') or parent is None
        if parent is not None and parent.unparsable:
            is_birth = False  # diff against the stale parsable version

        if ev_ok:
            for fqn in sorted(child.dups):
                identities[fid]['markers'].append(
                    {'i': i, 'kind': 'dup_shadow', 'fqn': fqn})

        mod_old = module_fqm(rel_p)
        mod_new = module_fqm(rel_c)
        is_move = kind == 'MOVE'

        if is_birth:
            for item in sorted(child.defs.values(), key=lambda d: d['local']):
                local = item['local']
                fqn = mod_new + '.' + local
                bump(item['ssid'], 1)
                if ev_ok:
                    cid = chain_of_local.get((fid, local)) or new_chain(
                        fid, local, None, None, None)
                    push({'i': i, 'kind': 'added', 'fqn': fqn,
                          'callees': item['callees'], 'ssid': item['ssid'],
                          'path': rel_c}, cid)
            live[fid] = child
            return

        parent_locals: dict = {}
        for item in parent.defs.values():
            parent_locals.setdefault(item['local'], item)
        child_locals: dict = {}
        for item in child.defs.values():
            child_locals.setdefault(item['local'], item)

        pairs: dict = {}   # new local -> old local (rename pairs, ssid equal)
        ren_cid: dict = {}  # old local -> the OLD chain's cid, for rename links
        by_ssid: dict = {}
        for local in sorted(set(parent_locals) - set(child_locals)):
            by_ssid.setdefault(parent_locals[local]['ssid'], local)
        for local in sorted(set(child_locals) - set(parent_locals)):
            old = by_ssid.get(child_locals[local]['ssid'])
            if old is not None and old not in pairs.values():
                pairs[local] = old

        for local in sorted(set(parent_locals) & set(child_locals)):
            p_item = parent_locals[local]
            c_item = child_locals[local]
            cid = chain_of_local.get((fid, local))
            if cid is None:
                continue  # shadowed defs are markers only
            if p_item['ssid'] != c_item['ssid']:
                # span fingerprint changed; keep live_index faithful even
                # when only the span bytes (comments) change
                bump(p_item['ssid'], -1)
                bump(c_item['ssid'], 1)
            if dump_masked(p_item['node']) == dump_masked(c_item['node']):
                # canonical bodies equal (LABEL RULE): comment-only span
                # changes are NOT body changes
                if is_move:
                    ev = {'i': i, 'kind': 'moved', 'fqn': mod_new + '.' + local,
                          'old_fqn': mod_old + '.' + local,
                          'callees': c_item['callees'], 'ssid': c_item['ssid'],
                          'path': rel_c, 'old_path': rel_p}
                    push(ev, cid)
                    pending.append(ev)
                continue
            if ev_ok:
                push({'i': i, 'kind': 'body_changed', 'fqn': mod_new + '.' + local,
                      'moved': is_move, 'callees': c_item['callees'],
                      'ssid': c_item['ssid'], 'path': rel_c,
                      'old_path': rel_p,
                      'old_ssid': p_item['ssid']}, cid)

        for local in sorted(set(parent_locals) - set(child_locals)):
            p_item = parent_locals[local]
            cid = chain_of_local.get((fid, local))
            mate = None
            for cand, at_old in pairs.items():
                if at_old == local:
                    mate = cand
                    break
            bump(p_item['ssid'], -1)
            if mate is not None:
                if cid is None:
                    continue
                old_chain = chain_by_cid[cid]
                prev_i = (old_chain['events'][-1]['i']
                          if old_chain['events'] else None)
                if ev_ok:
                    ev = {'i': i, 'kind': 'renamed_away',
                          'fqn': mod_old + '.' + local,
                          'renamed_to': mod_new + '.' + mate,
                          'callees': p_item['callees'], 'ssid': p_item['ssid'],
                          'old_prev_i': prev_i, 'path': rel_p}
                    push(ev, cid)
                    ren_cid[mate] = cid
                    del chain_of_local[(fid, local)]
                    pending.append(ev)
            else:
                if cid is None:
                    continue
                if ev_ok:
                    ev = {'i': i, 'kind': 'deleted',
                          'fqn': mod_old + '.' + local,
                          'callees': p_item['callees'], 'ssid': p_item['ssid'],
                          'path': rel_p}
                    push(ev, cid)
                    del chain_of_local[(fid, local)]
                    pending.append(ev)

        for local in sorted(set(child_locals) - set(parent_locals)):
            c_item = child_locals[local]
            old = pairs.get(local)
            bump(c_item['ssid'], 1)
            if not ev_ok:
                continue
            if old is not None:
                mate_cid = ren_cid.get(old)
                ev = {'i': i, 'kind': 'name_changed', 'fqn': mod_new + '.' + local,
                      'old_fqn': mod_old + '.' + old, 'callees': c_item['callees'],
                      'ssid': c_item['ssid'], 'path': rel_c, 'old_path': rel_p}
                pid = new_chain(fid, local, mate_cid, mod_old + '.' + old, None)
                if mate_cid is not None:
                    mate_chain = chain_by_cid[mate_cid]
                    away = (mate_chain['events'][-1]
                            if mate_chain['events'] and
                            mate_chain['events'][-1]['kind'] == 'renamed_away'
                            else None)
                    if away is not None and away.get('old_prev_i') is not None:
                        chain_by_cid[pid]['old_prev_i'] = away['old_prev_i']
                        ev['old_prev_i'] = away['old_prev_i']
                push(ev, pid)
            else:
                pid = new_chain(fid, local, None, None, None)
                push({'i': i, 'kind': 'added', 'fqn': mod_new + '.' + local,
                      'callees': c_item['callees'], 'ssid': c_item['ssid'],
                      'path': rel_c}, pid)

        live[fid] = child

    for i in range(n):
        bucket = at[i]
        if not bucket:
            continue
        for fid, bound in bucket:
            apply_boundary(fid, bound, i)
        for ev in pending:
            if ev is not None:
                ev['matches'] = live_index.get(ev.get('ssid'), 0)
        pending.clear()

    by_fid: dict = {}
    for chain in chains:
        by_fid.setdefault(chain['fid'], []).append(chain)
    identities_out = []
    for identity in identities:
        chain_list = sorted(by_fid.get(identity['id'], []), key=lambda c: c['cid'])
        identities_out.append({
            'id': identity['id'],
            'path': identity['path'],
            'ghost': identity['ghost'],
            'bounds': identity.get('bounds_summary', []),
            'markers': identity['markers'],
            'chains': chain_list,
        })
    return {'lineage': lineage, 'identities': identities_out}


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    repo_dir = sys.argv[1]
    out_path = sys.argv[2]
    repo_name = os.path.basename(os.path.abspath(repo_dir))
    data = label_repo(repo_dir, repo_name)
    n_events = sum(len(c['events']) for ident in data['identities']
                   for c in ident['chains'])
    n_chains = sum(len(ident['chains']) for ident in data['identities'])
    n_ghosts = sum(1 for ident in data['identities'] if ident['ghost'])
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or '.', exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as fh:
        json.dump({repo_name: data}, fh)
    print(f'[{repo_name}] wrote {out_path} chains={n_chains} '
          f'events={n_events} ghosts={n_ghosts}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
