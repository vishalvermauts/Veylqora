#include "veylqora/index/python_parser.hpp"

#include <catch2/catch_test_macros.hpp>

using veylqora::FactKind;
using veylqora::parse_python;
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
  const auto syms = parse_python("pkg.mod", src).symbols;
  REQUIRE(syms.size() == 6);
  CHECK(syms[0].kind == SymbolKind::module);
  CHECK(syms[0].fqn == "pkg.mod");
  CHECK(syms[1].fqn == "pkg.mod.top");
  CHECK(syms[1].kind == SymbolKind::function);
  CHECK(syms[2].fqn == "pkg.mod.A");
  CHECK(syms[2].kind == SymbolKind::klass);
  CHECK(syms[3].fqn == "pkg.mod.A.m");
  CHECK(syms[3].kind == SymbolKind::method);
  CHECK(syms[4].fqn == "pkg.mod.A.m.inner");
  CHECK(syms[4].kind == SymbolKind::function);
  CHECK(syms[5].fqn == "pkg.mod.A.am");
  CHECK(syms[5].kind == SymbolKind::method);
}

TEST_CASE("a definition's lines and body come from the syntax tree") {
  const auto syms = parse_python("m", "def top():\n    pass\n\n\nx = 1\n").symbols;
  REQUIRE(syms.size() == 2);
  CHECK(syms[1].start_line == 1);
  CHECK(syms[1].end_line == 2);
  CHECK(syms[1].body == "def top():\n    pass");
}

TEST_CASE("a def inside a string is not a symbol") {
  // The indentation scanner this replaced would have reported `inner`.
  const char* src = "doc = '''\ndef inner():\n    pass\n'''\n";
  const auto syms = parse_python("m", src).symbols;
  CHECK(syms.size() == 1);  // only the module
}

TEST_CASE("comments inside a body belong to it") {
  const char* src =
      "class A:\n"
      "    def m(self):\n"
      "        x = 1\n"
      "        # note\n"
      "        return x\n";
  const auto syms = parse_python("m", src).symbols;
  REQUIRE(syms.size() == 3);
  CHECK(syms[2].end_line == 5);
}

TEST_CASE("repeated names in one scope get distinct overloads") {
  const auto syms = parse_python("m", "def f():\n    pass\ndef f():\n    pass\n").symbols;
  REQUIRE(syms.size() == 3);
  CHECK(syms[1].fqn == syms[2].fqn);
  CHECK(syms[1].overload == 0);
  CHECK(syms[2].overload == 1);
}

TEST_CASE("CRLF line endings produce the same symbols") {
  const auto unix_syms = parse_python("m", "def f():\n    return 1\n").symbols;
  const auto dos_syms = parse_python("m", "def f():\r\n    return 1\r\n").symbols;
  REQUIRE(dos_syms.size() == unix_syms.size());
  CHECK(dos_syms[1].end_line == unix_syms[1].end_line);
  CHECK(dos_syms[1].body == unix_syms[1].body);
}

TEST_CASE("calls, bases and imports are recorded as facts with their caller") {
  const char* src =
      "import os.path\n"
      "from pkg.util import helper as h\n"
      "class Child(Base):\n"
      "    def run(self):\n"
      "        self.step()\n"
      "        h(1)\n";
  const auto parsed = parse_python("pkg.core", src);
  bool import_os = false;
  bool import_util = false;
  bool inherits_base = false;
  bool self_step = false;
  bool call_h = false;
  for (const auto& f : parsed.facts) {
    if (f.kind == FactKind::import && f.name == "os.path") import_os = true;
    if (f.kind == FactKind::import && f.name == "pkg.util") import_util = true;
    if (f.kind == FactKind::inherits && f.name == "Base" && f.qualifier.empty())
      inherits_base = true;
    if (f.kind == FactKind::call && f.name == "step" && f.qualifier == "self") {
      self_step = true;
      CHECK(f.scope == "pkg.core.Child");
      CHECK(parsed.symbols[f.src].fqn == "pkg.core.Child.run");
    }
    if (f.kind == FactKind::call && f.name == "h" && f.qualifier.empty()) call_h = true;
  }
  CHECK(import_os);
  CHECK(import_util);
  CHECK(inherits_base);
  CHECK(self_step);
  CHECK(call_h);
}

TEST_CASE("from-imports bind local names to fully qualified targets") {
  const auto parsed =
      parse_python("pkg.core", "from pkg.util import helper as h, other\n");
  bool alias = false;
  bool plain = false;
  for (const auto& b : parsed.bindings) {
    if (b.local == "h" && b.target == "pkg.util.helper") alias = true;
    if (b.local == "other" && b.target == "pkg.util.other") plain = true;
  }
  CHECK(alias);
  CHECK(plain);
}

TEST_CASE("relative imports resolve against the importing module's package") {
  // pkg.sub.core: one dot is the current package pkg.sub, two dots climb to pkg.
  const auto one = parse_python("pkg.sub.core", "from .util import helper\n");
  const auto two = parse_python("pkg.sub.core", "from ..base import Thing\n");
  bool one_ok = false;
  bool two_ok = false;
  for (const auto& b : one.bindings) one_ok = one_ok || b.target == "pkg.sub.util.helper";
  for (const auto& b : two.bindings) two_ok = two_ok || b.target == "pkg.base.Thing";
  CHECK(one_ok);
  CHECK(two_ok);
}

TEST_CASE("syntax errors are reported, and parsing still yields the valid parts") {
  const auto parsed = parse_python("m", "def good():\n    return 1\n\ndef broken(:\n");
  CHECK(parsed.has_syntax_errors);
  bool found_good = false;
  for (const auto& s : parsed.symbols) found_good = found_good || s.fqn == "m.good";
  CHECK(found_good);
}

TEST_CASE("empty source yields only the module symbol and no errors") {
  const auto parsed = parse_python("m", "");
  CHECK(parsed.symbols.size() == 1);
  CHECK_FALSE(parsed.has_syntax_errors);
  CHECK(parsed.facts.empty());
}

TEST_CASE("deeply nested input is cut off instead of exhausting the stack") {
  std::string src;
  for (int i = 0; i < 2000; ++i) src += "(";
  const auto parsed = parse_python("m", src);
  CHECK(parsed.has_syntax_errors);
}
