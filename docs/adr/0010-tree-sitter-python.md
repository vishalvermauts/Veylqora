# ADR-0010 — Python parsing with tree-sitter; edge model for the code graph

- **Status:** accepted (M1.3a)
- **Date:** 2026-10-20

## Context

The slice extractor (ADR-0008) finds `def` and `class` lines by indentation.
It cannot see calls, imports or inheritance, and it mis-reads a `def` inside a
string. The code graph (M1.3a) needs all of those, so it needs a real parser.

## Decision

- **Parser: tree-sitter v0.27.1** (MIT, C library, incremental, error-tolerant).
- **Grammar: tree-sitter-python v0.25.0** (MIT). Its `parser.c` and `scanner.c`
  are compiled into our library directly. Its CMake generation step is not used.
- **Both are pinned by tag in `cmake/Dependencies.cmake`.** Verified in the
  pinned tags: `ts_parser_set_language`, `ts_node_child_by_field_name`,
  `ts_node_has_error`, and the grammar field names `name`, `superclasses`,
  `function`, `object`, `attribute`, `module_name`, `alias`.
- **Error tolerance.** A file with syntax errors is still parsed. Its symbols
  are indexed, the file is counted as partial, and a warning is logged. A
  recursion guard stops descent at depth 512, so hostile nesting cannot exhaust
  the stack.

## Edge model

The graph stores edges between symbols. Each edge carries a **resolution
level**, so callers can tell confirmed facts from guesses:

| Level | Meaning |
|---|---|
| `resolved` | The target is confirmed: the name is bound by an import in this file, defined in this file, or is `self.` inside the same class |
| `heuristic` | A name match with no proof: a method called on an unknown receiver, or a name defined in several places. At most 5 candidates are linked |
| `unresolved` | No candidate exists in the project, for example `len()` or an external library |

Edge kinds: `contains` (class or module to member), `imports`, `calls`,
`inherits`. `references` (variable reads, attribute access) is deferred to M1.3b;
it is not claimed here.

## Process

1. Parsing is per file. Each file yields symbols, *facts* (call, inherit and
   import sites, with the caller and the raw name), and *bindings* (local name
   to fully qualified target).
2. Facts are stored. Resolution is a separate, whole-project pass that rebuilds
   the `edge` table from stored facts. No file is re-parsed to resolve a name.
   The pass runs only when a run changed something, and only after a run that
   was not cancelled.

## Known limitations

- Dynamic dispatch, `getattr`, decorators that rewrite functions, and runtime
  imports are not modelled. Their calls are heuristic or unresolved.
- Package `__init__.py` modules are named `pkg.__init__`, so a re-export from
  `pkg` is not followed.
- The resolution pass loads the whole project's symbols into memory. This is
  measured in M3.2 and will be bounded there if it matters.

## Consequences

- Calls to project-internal functions are confirmed when the import is visible,
  and reported as guesses otherwise. This is the honest level the plan asks for
  (§6.4).
- The public extraction API changes from a list of symbols to a parse result.
  The slice tests are updated accordingly.
