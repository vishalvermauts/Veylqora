#pragma once

#include "veylqora/core/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace veylqora {

/// One definition found in a source file, in any supported language.
struct SourceSymbol {
  std::string name;
  std::string fqn;
  SymbolKind kind;
  std::size_t start_line;  // 1-based
  std::size_t end_line;    // 1-based, inclusive
  std::string body;        // source text of the definition (empty for a module)
  std::uint32_t overload;  // disambiguates repeated (fqn, kind) within one file
};

enum class FactKind : std::uint8_t { call, inherits, import };

/// A reference site found in source. Resolved against the whole project later.
struct SourceFact {
  FactKind kind;
  std::size_t src;        // index into SourceFile::symbols of the enclosing scope
  std::string name;       // callee, base class, or imported module
  std::string qualifier;  // "" for a bare name, "self", or the receiver's text
  std::string scope;      // fqn of the nearest enclosing class (for self.m())
  std::size_t line;       // 1-based
};

/// A local name introduced by an import (or re-export), and what it refers to.
struct SourceBinding {
  std::string local;
  std::string target;  // fully qualified, e.g. "pkg.util.helper"
};

struct SourceFile {
  std::vector<SourceSymbol> symbols;  // symbols[0] is the module itself
  std::vector<SourceFact> facts;
  std::vector<SourceBinding> bindings;
  bool has_syntax_errors = false;
};

}  // namespace veylqora
