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
file(DOWNLOAD "${TS_PY_BASE}/src/tree_sitter/alloc.h" "${TS_PY_DIR}/src/tree_sitter/alloc.h"
     EXPECTED_HASH SHA256=b29c1c9fb7cc82f58c84b376df1297d6e2737a1d655fd356db0859e3c29c2fea)
file(DOWNLOAD "${TS_PY_BASE}/src/tree_sitter/array.h" "${TS_PY_DIR}/src/tree_sitter/array.h"
     EXPECTED_HASH SHA256=5bdf6ed1a78e3409fd443e085ca967a64c188a5d082aaf7f819bccd53a471c94)
add_library(tree_sitter_python STATIC
  "${TS_PY_DIR}/src/parser.c"
  "${TS_PY_DIR}/src/scanner.c")
target_include_directories(tree_sitter_python SYSTEM PUBLIC "${TS_PY_DIR}/src")
target_link_libraries(tree_sitter_python PUBLIC tree-sitter)

# ---- JavaScript and TypeScript grammars (M1.3b, ADR-0019) -------------------
# Same approach as Python: the generated parser.c, scanner.c and the headers
# they include are fetched at a pinned tag and verified by SHA-256. The
# TypeScript grammar includes ../../common/scanner.h, so that file is placed at
# the location the include expects.
set(TS_JS_BASE "https://raw.githubusercontent.com/tree-sitter/tree-sitter-javascript/v0.25.0")
set(TS_JS_DIR "${CMAKE_BINARY_DIR}/third_party/tree_sitter_javascript")
file(DOWNLOAD "${TS_JS_BASE}/src/parser.c" "${TS_JS_DIR}/src/parser.c"
     EXPECTED_HASH SHA256=67209ca7ef6e1a4f74e29e48b5928455f892fe1821a3960fbcd62f4e972f7384)
file(DOWNLOAD "${TS_JS_BASE}/src/scanner.c" "${TS_JS_DIR}/src/scanner.c"
     EXPECTED_HASH SHA256=b3d3f64284d97bf80749c026862427782cf7ecc0b7dc094e6698ab311c9a42c7)
file(DOWNLOAD "${TS_JS_BASE}/src/tree_sitter/parser.h" "${TS_JS_DIR}/src/tree_sitter/parser.h"
     EXPECTED_HASH SHA256=180b893c8734778fd32f372dfbc27bd6ad1cd2221f26150b31256ff6716320d2)
file(DOWNLOAD "${TS_JS_BASE}/src/tree_sitter/alloc.h" "${TS_JS_DIR}/src/tree_sitter/alloc.h"
     EXPECTED_HASH SHA256=b29c1c9fb7cc82f58c84b376df1297d6e2737a1d655fd356db0859e3c29c2fea)
file(DOWNLOAD "${TS_JS_BASE}/src/tree_sitter/array.h" "${TS_JS_DIR}/src/tree_sitter/array.h"
     EXPECTED_HASH SHA256=5bdf6ed1a78e3409fd443e085ca967a64c188a5d082aaf7f819bccd53a471c94)
add_library(tree_sitter_javascript STATIC
  "${TS_JS_DIR}/src/parser.c"
  "${TS_JS_DIR}/src/scanner.c")
target_include_directories(tree_sitter_javascript PRIVATE "${TS_JS_DIR}/src")
target_link_libraries(tree_sitter_javascript PUBLIC tree-sitter)

set(TS_TS_BASE "https://raw.githubusercontent.com/tree-sitter/tree-sitter-typescript/v0.23.2")
set(TS_TS_DIR "${CMAKE_BINARY_DIR}/third_party/tree_sitter_typescript")
file(DOWNLOAD "${TS_TS_BASE}/typescript/src/parser.c" "${TS_TS_DIR}/typescript/src/parser.c"
     EXPECTED_HASH SHA256=74fe453edd70f4eae9af0a1050cbd7943d8971d59165b6aaebbaa0a0b716d1aa)
file(DOWNLOAD "${TS_TS_BASE}/typescript/src/scanner.c" "${TS_TS_DIR}/typescript/src/scanner.c"
     EXPECTED_HASH SHA256=9125013b42cb888379d9be909f1d73dfb75a37626c2cdbf4122718a2b431a6d3)
file(DOWNLOAD "${TS_TS_BASE}/typescript/src/tree_sitter/parser.h" "${TS_TS_DIR}/typescript/src/tree_sitter/parser.h"
     EXPECTED_HASH SHA256=a1f6ef161fbaf48a0e10fca90ef5290a062462b307b3898aa562993853b9f80a)
file(DOWNLOAD "${TS_TS_BASE}/typescript/src/tree_sitter/alloc.h" "${TS_TS_DIR}/typescript/src/tree_sitter/alloc.h"
     EXPECTED_HASH SHA256=b29c1c9fb7cc82f58c84b376df1297d6e2737a1d655fd356db0859e3c29c2fea)
file(DOWNLOAD "${TS_TS_BASE}/typescript/src/tree_sitter/array.h" "${TS_TS_DIR}/typescript/src/tree_sitter/array.h"
     EXPECTED_HASH SHA256=4ff743903dc46f5db6aa54f31c6b4d160a8a9779e5b2ab1ee59ae7ebcd850ea1)
file(DOWNLOAD "${TS_TS_BASE}/common/scanner.h" "${TS_TS_DIR}/common/scanner.h"
     EXPECTED_HASH SHA256=da66ef2bd14a3f7ea743e25ba068c6c9aae2c3509db200ff80c4a0e6116e564c)
add_library(tree_sitter_typescript STATIC
  "${TS_TS_DIR}/typescript/src/parser.c"
  "${TS_TS_DIR}/typescript/src/scanner.c")
target_include_directories(tree_sitter_typescript PRIVATE "${TS_TS_DIR}/typescript/src")
target_link_libraries(tree_sitter_typescript PUBLIC tree-sitter)

# ---- cpp-httplib (HTTP transport) and nlohmann/json (wire format), both MIT ----
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.62.0
  GIT_SHALLOW    TRUE
  SYSTEM)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE
  SYSTEM)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

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
