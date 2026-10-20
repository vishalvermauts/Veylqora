# Dependency strategy: CMake FetchContent with pinned git tags, no package
# manager. Rationale and reversal criteria in docs/adr/0002-dependencies.md.
#
# Every dependency is pinned to an immutable tag and acquired with
# SYSTEM so its warnings never reach our -Werror build.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- SQLite -----------------------------------------------------------------
# Authoritative metadata store. Prefer the system library when present (fast
# CI, distro security updates); the amalgamation fallback is added in Module
# 1.1 only if a supported platform turns out to lack it.
find_package(SQLite3 REQUIRED)

# ---- SHA-256 ----------------------------------------------------------------
# Symbol identity and content fingerprints need a collision-resistant digest.
# picosha2 is a single MIT header, which is cheaper than linking OpenSSL for
# one function and safer than hand-rolling a digest.
FetchContent_Declare(picosha2
  GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
  GIT_TAG        27fcf6979298949e8a462e16d09a0351c18fcaf2
  GIT_SHALLOW    FALSE
  SYSTEM)
# PicoSHA2 defines its own `picosha2` interface target; SYSTEM above marks its
# interface include directories as system, so we consume it as-is.
FetchContent_MakeAvailable(picosha2)

# ---- tree-sitter and the Python grammar (M1.3a, ADR-0010) --------------------
# The grammar is compiled directly from its generated parser.c and scanner.c.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(tree_sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  GIT_TAG        v0.27.1
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(tree_sitter)

# The grammar's own CMake regenerates parser.c with the tree-sitter CLI, which
# this build does not need. The three generated files are fetched at the pinned
# tag and verified by SHA-256 instead, so the build depends only on their bytes.
set(TS_PY_BASE "https://raw.githubusercontent.com/tree-sitter/tree-sitter-python/v0.25.0")
set(TS_PY_DIR "${CMAKE_BINARY_DIR}/third_party/tree_sitter_python")
file(DOWNLOAD "${TS_PY_BASE}/src/parser.c" "${TS_PY_DIR}/src/parser.c"
     EXPECTED_HASH SHA256=a895f10b3cf7b2608f3283b43cd5cfed70971c7ee4a0136abbaaccbc4a7a25e0)
file(DOWNLOAD "${TS_PY_BASE}/src/scanner.c" "${TS_PY_DIR}/src/scanner.c"
     EXPECTED_HASH SHA256=6db82134ac2d4c90a1a1475487a625cface02662ebda9b7478cad9c7147e9afe)
file(DOWNLOAD "${TS_PY_BASE}/src/tree_sitter/parser.h" "${TS_PY_DIR}/src/tree_sitter/parser.h"
     EXPECTED_HASH SHA256=180b893c8734778fd32f372dfbc27bd6ad1cd2221f26150b31256ff6716320d2)
add_library(tree_sitter_python STATIC
  "${TS_PY_DIR}/src/parser.c"
  "${TS_PY_DIR}/src/scanner.c")
target_include_directories(tree_sitter_python SYSTEM PUBLIC "${TS_PY_DIR}/src")
target_link_libraries(tree_sitter_python PUBLIC tree-sitter)

# ---- Catch2 (tests only) ----------------------------------------------------
if(VEYLQORA_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.11.0
    GIT_SHALLOW    TRUE
    SYSTEM)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
  include(Catch)
endif()

# Dependencies introduced by later modules, each gated behind the module that
# needs it so the Module 1.1 build stays small:
#   Module 1.3  tree-sitter + grammars   (parsing)
#   Module 2.1  usearch                  (vector index)
# They are declared here when that module lands, never earlier.
