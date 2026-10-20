# Fixture provenance

`python/` is a 10-file Python project written for Veylqora's tests. It is not
copied from any third-party repository. It is licensed under the project's
Apache-2.0 licence (see `LICENSE`).

The files are deliberately small and cover: top-level functions, classes,
methods, a nested class, a nested function, a dunder method, an `async def`,
and two files that share no names. Expected symbols are labelled by hand in
`tests/integration/test_slice.cpp`.

## graph/

Three hand-written modules for the code graph (M1.3a). The expected symbols and
edges are labelled by hand from reading the source, in
`tests/integration/test_graph.cpp`, and were not copied from program output.
The labels cover every symbol and every edge the graph should contain for this
fixture, so precision and recall are both measured over a complete answer key.
