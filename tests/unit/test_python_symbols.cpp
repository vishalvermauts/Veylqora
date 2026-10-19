#include "veylqora/index/python_symbols.hpp"

#include <catch2/catch_test_macros.hpp>

using veylqora::extract_python_symbols;
using veylqora::SymbolKind;

TEST_CASE("functions, classes, methods and nested functions get dotted names") {
  const char* src =
      "def top():\n"
      "    pass\n"
      "\n"
      "class A:\n"
      "    def m(self):\n"
      "        def inner():\n"
      "            return 1\n"
      "        return inner\n"
      "\n"
      "    async def am(self):\n"
      "        pass\n";
  const auto syms = extract_python_symbols("pkg.mod", src);
  REQUIRE(syms.size() == 5);
  CHECK(syms[0].fqn == "pkg.mod.top");
  CHECK(syms[0].kind == SymbolKind::function);
  CHECK(syms[1].fqn == "pkg.mod.A");
  CHECK(syms[1].kind == SymbolKind::klass);
  CHECK(syms[2].fqn == "pkg.mod.A.m");
  CHECK(syms[2].kind == SymbolKind::method);
  CHECK(syms[3].fqn == "pkg.mod.A.m.inner");
  CHECK(syms[3].kind == SymbolKind::function);
  CHECK(syms[4].fqn == "pkg.mod.A.am");
  CHECK(syms[4].kind == SymbolKind::method);
}

TEST_CASE("a body ends at its last code line, not at trailing blank lines") {
  const auto syms = extract_python_symbols("m", "def top():\n    pass\n\n\nx = 1\n");
  REQUIRE(syms.size() == 1);
  CHECK(syms[0].start_line == 1);
  CHECK(syms[0].end_line == 2);
  CHECK(syms[0].body == "def top():\n    pass\n");
}

TEST_CASE("comments inside a body do not end it") {
  const char* src =
      "class A:\n"
      "    def m(self):\n"
      "        x = 1\n"
      "        # note\n"
      "        return x\n";
  const auto syms = extract_python_symbols("m", src);
  REQUIRE(syms.size() == 2);
  CHECK(syms[1].end_line == 5);
}

TEST_CASE("repeated names in one scope get distinct overloads") {
  const auto syms = extract_python_symbols("m", "def f():\n    pass\ndef f():\n    pass\n");
  REQUIRE(syms.size() == 2);
  CHECK(syms[0].fqn == syms[1].fqn);
  CHECK(syms[0].overload == 0);
  CHECK(syms[1].overload == 1);
}

TEST_CASE("CRLF line endings produce the same symbols") {
  const auto unix_syms = extract_python_symbols("m", "def f():\n    return 1\n");
  const auto dos_syms = extract_python_symbols("m", "def f():\r\n    return 1\r\n");
  REQUIRE(dos_syms.size() == unix_syms.size());
  CHECK(dos_syms[0].end_line == unix_syms[0].end_line);
  CHECK(dos_syms[0].body == unix_syms[0].body);
}

TEST_CASE("source with no definitions yields no symbols") {
  CHECK(extract_python_symbols("m", "").empty());
  CHECK(extract_python_symbols("m", "x = 1\n# def not_a_def\n").empty());
}
