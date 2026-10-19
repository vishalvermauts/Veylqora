# Security

Status: threat model for M1.1 (the vertical slice). It is refreshed in M3.3.

## Trust boundaries

- **Indexed repository: untrusted.** File names, contents, symlinks and text
  in comments or strings are attacker-controlled in general.
- **Daemon and CLI: trusted.** They run with the invoking user's rights.
- **Network: not present yet.** The HTTP API, and with it the authentication
  rules in the plan (§10.2), arrive with the daemon in M1.5.

## Controls in place for M1.1

| Threat | Control | Where | Tested by |
|---|---|---|---|
| Symlink pointing outside the project is read | Symlinks are skipped, never followed | `src/index/indexer.cpp` | not yet (M1.2 adds symlink tests) |
| Path traversal through stored file names | Stored paths are relative to the canonical root and produced by the walker, not taken from input | `src/index/indexer.cpp` | by construction; traversal corpus in M3.3 |
| Oversized file exhausts memory | Files over 1 MiB are skipped with a warning | `src/index/indexer.cpp` | `kMaxFileBytes` path, covered in M1.2 |
| SQL injection through names or paths | All values are bound parameters; no SQL is built from input | `src/storage/database.cpp` | by construction |
| Repository code is executed during indexing | Never. The indexer reads bytes and parses text | whole indexer | by construction |
| Partial writes corrupt the database | One transaction per index run; rollback on any error | `Transaction` | rollback is exercised by the error path; crash test in M1.4 |
| Projects read each other's symbols | Every row carries `project_id`; queries filter on it | `src/index/indexer.cpp` | `projects sharing one database` integration test |
| Instructions inside source files are obeyed | Source text is stored and returned as data; nothing interprets it | whole indexer | by construction; LLM-assisted paths in M2 are gated (plan §10.4) |

## Known gaps (accepted for M1.1, tracked)

- **No secret redaction yet.** Indexed source, including any secrets it
  contains, is stored in the database as-is. Redaction at ingest is M1.2.
- **No UTF-8 validation.** Invalid byte sequences are stored as bytes. M1.2.
- **Single writer by convention only.** Two CLI processes can write the same
  database concurrently; SQLite serialises them, but there is no lock file yet.
  The daemon (M1.5) makes the server the only writer (ADR-0004).
- **Database file permissions** follow the umask of the invoking user. Not yet
  restricted.
- **Symlink handling is by skipping.** A symlinked directory is not walked.
  Path canonicalisation of the root is done, but per-file canonical checks
  are not yet implemented; M1.2 adds them with tests.
