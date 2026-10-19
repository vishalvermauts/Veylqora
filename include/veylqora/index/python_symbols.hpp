#pragma once

#include "veylqora/core/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace veylqora {

struct PySymbol {
  std::string name;
  std::string fqn;
  SymbolKind kind;
  std::size_t start_line;  // 1-based, the `def` or `class` line
  std::size_t end_line;    // 1-based, inclusive, last code line of the body
  std::string body;
  std::uint32_t overload;  // disambiguates repeated (fqn, kind) within one file
};

/// Extracts functions, classes and methods from Python source by indentation.
///
/// This is a syntax-level extractor for the vertical slice (ADR-0008). It does
/// not resolve names, does not understand multi-line strings (a `def` inside a
/// docstring is taken as code), and ignores decorators for the start line.
/// Tree-sitter replaces it in M1.3a, with precision and recall measured there.
std::vector<PySymbol> extract_python_symbols(std::string_view module,
                                             std::string_view source);

}  // namespace veylqora
