# ADR-0019: JavaScript and TypeScript code graph

Status: accepted (M1.3b)

## Context

Phase 1 indexed Python only. Most agent-facing repositories mix Python with
JavaScript or TypeScript, so the graph must cover both with the same contract:
the same symbol identity, facts, resolution levels and retrieval rows as Python
(ADR-0003, ADR-0010, ADR-0014).

## Decisions

1. **Grammars.** tree-sitter-javascript v0.25.0 and tree-sitter-typescript v0.23.2
   are vendored the same way as the Python grammar: generated `parser.c` and
   `scanner.c`, and the headers they include, fetched at a pinned tag and
   checked by SHA-256 at configure time. The TypeScript scanner includes
   `../../common/scanner.h`, which is vendored alongside it.
2. **One walker per grammar family, shared helpers.** `src/index/tree_sitter_common.hpp`
   holds the helpers both walkers use (comment stripping, line numbers, node
   lookup). The structs both parsers return are `SourceFile`, `SourceSymbol`,
   `SourceFact` and `SourceBinding` (`index/source_model.hpp`), so the resolver
   is unchanged.
3. **Dialects.** `.js`, `.mjs`, `.cjs` and `.jsx` use the JavaScript grammar.
   `.ts`, `.mts` and `.cts` use the TypeScript grammar. `.tsx` is not indexed
   yet: it needs the TSX grammar, which is a separate vendored parser.
4. **Module names.** `src/api/routes.ts` is module `src.api.routes`. An index
   file names its directory: `src/api/index.ts` is `src.api`. Module names use
   the same dotted form as Python, so the resolver's fully qualified lookups work
   unchanged.
5. **Import resolution.** `./x` and `../x` resolve against the importing file's
   directory and drop a trailing extension (`.js` in `../lib/geometry.js` names
   `lib.geometry`). Bare specifiers keep their spelling. A default import binds
   `module.default`. A namespace import binds the module itself, so `ns.f()`
   resolves through the module.
6. **Re-exports.** `export { a as b } from "m"` binds `b` to `m.a`, and
   `export * as ns from "m"` binds `ns` to `m`. Re-exports follow the same
   bounded hop rule as Python (ADR-0010).
7. **Calls and inheritance.** `f()` and `obj.m()` are calls, and `new C()` is a
   call of `C`. `this.m()` is recorded with the qualifier `self`, so the
   resolver treats it as a call within the enclosing class, as it does for
   Python's `self.m()`. `class A extends B` and `extends ns.B` are inheritance.
8. **Symbols.** Function declarations, classes, methods, and `const f = () => ...`
   or `const f = function () {...}` are symbols. A function or method takes
   the kind it has in Python: a method when its enclosing scope is a class,
   otherwise a function.
9. **Definition fingerprint.** The name is removed as the first whole-word
   occurrence in the body, so a rename or move of an unchanged body keeps its
   fingerprint (ADR-0014).

## Limitations (recorded, not hidden)

- `.tsx` files are not indexed, so React components written in TSX are not in
  the graph.
- CommonJS `require()` is not modelled as an import. Calls through a
  `require()`d name fall back to the heuristic name match.
- `export * from "m"` (without `as`) is not recorded, because it binds no
  names this file can list.
- A plain `export { x }` of a local name is not recorded. Recording it would
  overwrite an import binding of the same name in this file.
- TypeScript interfaces, type aliases and enums are not symbols.
- Dynamic dispatch, computed member names and `super.m()` make no claim.
- Module names share the resolver's global namespace with Python names. A
  Python package and a JavaScript module with the same dotted name would collide.

## Evaluation

Each language has its own labelled fixture under `tests/fixtures/typescript` and
`tests/fixtures/javascript`. The labels cover the call and inheritance edges
that the source makes explicit, and each label was checked by reading the
fixture. Precision is measured over edges that name a project symbol; calls
into external packages make no claim and are not scored.

The gate was fixed before the first run:

| Language | Precision | Recall |
|---|---|---|
| TypeScript | ≥ 0.90 | ≥ 0.85 |
| JavaScript | ≥ 0.90 | ≥ 0.85 |

Scores are reported separately for each language. No shared fixture blends them.

## Consequences

- The build fetches two more grammars at configure time, pinned and verified.
- Adding `.tsx` later needs a third vendored grammar and an ADR amendment.
