# ADR-0009 — Ingestion policy: git-aware listing, classification, batching

- **Status:** accepted (M1.2)
- **Date:** 2026-10-20

## Context

M1.2 must index real repositories correctly: respect `.gitignore`, skip what
cannot be indexed safely, survive interruption, and never lose valid data
because of a transient failure. Several of these choices carry security weight,
because the indexed repository is untrusted input (SECURITY.md).

## Decisions

1. **Candidates come from git when available.** Inside a work tree the list is
   `git ls-files -z --cached --others --exclude-standard`. This honours nested
   `.gitignore`, `.git/info/exclude` and the global excludes file, with no
   parser of our own. Outside git, or where git cannot start, a recursive walk
   skips `.git` directories. Windows has no process spawning in this build, so
   it always walks; that is recorded as a limitation.

2. **Git is run without a shell and with untrusted-config defences.** Arguments
   are passed as an argv array. `core.fsmonitor` is disabled, because a
   repository's own config can name a program that git will run. This is
   tested only by inspection; a test that places such a config would prove it.

3. **Each file is classified before it is parsed.** Symlinks are never
   followed. Non-regular files, files over 1 MiB, files containing NUL bytes
   (binary), and files that are not well-formed UTF-8 are skipped, and any rows
   they had are removed, because their stored symbols no longer describe them.

4. **Transient read errors keep old rows.** A file that cannot be read this
   run is not deleted from the index. Deleting on a read error would turn a
   permissions glitch into lost data.

5. **Work is committed in batches (200 files by default).** Each batch is one
   transaction. Cancellation is checked between files, so an interrupted run
   keeps every committed batch and a re-run skips them, because their content
   hashes match. The batch size bounds lock hold time and memory. It is not a
   thread pool: parsing is cheap relative to I/O at this stage, and a queue
   would be speculative until profiling shows it is needed (ADR-0008).

6. **Removals happen only when a run completes.** A cancelled run does not
   delete rows for files it did not reach.

7. **Renames are delete plus add.** The symbol identity includes the path
   (ADR-0003), so a moved file gets new symbol identities. Carrying evidence
   across a rename is M2.3 work, not M1.2.

8. **Language is decided by extension.** Only `.py` is indexed in v0.1.

## Measured on public repositories

`scripts/corpus.sh`, run in the cloud sandbox against shallow clones
(`docs/research/corpus-m1.2.txt`):

| Repository | Python files | Symbols | First index | Re-index | Database |
|---|---|---|---|---|---|
| psf/requests | 37 | 807 | 0.0 s | 0.0 s | 516 KB |
| pallets/flask | 83 | 1,658 | 0.1 s | 0.0 s | 995 KB |
| django/django | 2,933 | 44,029 | 8.1 s | 0.3 s | 27 MB |

Each re-index reports every file as unchanged. A known class is found in the
expected file in all three. The first run on django is roughly 180 files per
second, below the 500 files/s provisional target in plan §4. The target is
recorded as not yet met, and the cost is attributed to the per-file statement
preparation and the indexer's single writer, to be confirmed by profiling in M3.2.

## Consequences

- A changed `.gitignore` takes effect on the next run, because ignored files
  simply stop appearing in the listing and are removed.
- Windows indexing is correct but does not honour `.gitignore`.
- `scripts/corpus.sh` needs network access to GitHub, so it is run by hand,
  not in gating CI.
