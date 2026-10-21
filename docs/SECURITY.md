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
| Symlink pointing outside the project is read | Symlinks are skipped, never followed | `src/ingestion/source.cpp` | `symlinks are never followed` integration test |
| Path traversal through stored file names | Stored paths are relative to the canonical root and produced by the walker, not taken from input | `src/index/indexer.cpp` | by construction; traversal corpus in M3.3 |
| Oversized file exhausts memory | Files over 1 MiB are skipped with a warning | `src/ingestion/source.cpp` | size classification, skip counted in stats |
| Git runs repository-configured programs | Git is run with argv (no shell) and `core.fsmonitor=false` (ADR-0009) | `src/ingestion/process.cpp` | by inspection; a planted-config test is still to be written |
| Invalid text breaks parsing | Non-UTF-8 and NUL-containing files are skipped | `src/ingestion/source.cpp` | `is_valid_utf8` unit tests, binary and invalid-UTF-8 integration tests |
| SQL injection through names or paths | All values are bound parameters; no SQL is built from input | `src/storage/database.cpp` | by construction |
| Repository code is executed during indexing | Never. The indexer reads bytes and parses text | whole indexer | by construction |
| Partial writes corrupt the database | One transaction per index run; rollback on any error | `Transaction` | rollback is exercised by the error path; crash test in M1.4 |
| Projects read each other's symbols | Every row carries `project_id`; queries filter on it | `src/index/indexer.cpp` | `projects sharing one database` integration test |
| Instructions inside source files are obeyed | Source text is stored and returned as data; nothing interprets it | whole indexer | by construction; LLM-assisted paths in M2 are gated (plan §10.4) |

## Controls added in M3.3

| Threat | Control | Where | Tested by |
|---|---|---|---|
| Static web mount serves a file outside its directory | Encoded and double-encoded `../` requests never return a file outside the mounted directory | `src/server/server.cpp` (cpp-httplib mount) | `static web mount: traversal to a file outside the root is never served` (11 variants) |
| Hostile path-like API inputs cause a server error or echo file content | Such inputs return a 4xx or an empty result, and file content is never echoed | `src/api/service.cpp`, `src/server/server.cpp` | `API path inputs that look like traversal fail cleanly` |
| Crash on malformed Python, JavaScript or TypeScript source | Parsers return partial results or a reported error | `src/index/python_parser.cpp`, `src/index/javascript_parser.cpp` | `fuzz_python_parser`, `fuzz_javascript_parser` (libFuzzer, ASan and UBSan) |
| Crash or hang on hostile search queries | Queries are bounded and malformed ones return a 4xx | `src/retrieval/search.cpp` | `fuzz_retrieval_query` |
| Crash on malformed or hostile JSON request bodies | Bodies are parsed and validated; failures are 400s | `src/api/service.cpp` | `fuzz_memory_json` |
| Credentials committed to the repository | History scanned for provider token patterns and credential-like file names | repository | pattern scan of 169 commits and 245 paths; no match |

The fuzz targets are a pattern-based check, not a substitute for a dedicated
secret scanner, and the history scan covers token formats only.

## Known gaps (accepted for M1.1, tracked)

- **No secret redaction yet.** Indexed source, including any secrets it
  contains, is stored in the database as-is. Redaction at ingest is planned,
  not yet scheduled.
- **Single writer by convention only.** Two CLI processes can write the same
  database concurrently; SQLite serialises them, but there is no lock file yet.
  The daemon (M1.5) makes the server the only writer (ADR-0004).
- **Database file permissions** follow the umask of the invoking user. Not yet
  restricted.
- **Symlinks are skipped, not resolved.** A symlinked directory is not walked.
- **Secret redaction is still absent** (see above); it belongs with ingest-time
  redaction in a later module.
