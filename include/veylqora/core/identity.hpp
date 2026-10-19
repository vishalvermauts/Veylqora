#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace veylqora {

/// What kind of entity a symbol is. Part of symbol identity, because a class
/// and a function of the same qualified name are different symbols.
enum class SymbolKind : std::uint8_t {
  file,
  module,
  klass,
  function,
  method,
  field,
};

std::string_view to_string(SymbolKind kind) noexcept;

/// The identity-bearing fields of a symbol.
///
/// Identity deliberately excludes line numbers and body text: a symbol that
/// moves or is re-indented keeps its identity, and a symbol whose body changes
/// keeps its identity while its fingerprint changes. Keeping those two facts
/// separate is what lets evidence survive refactoring instead of being
/// invalidated by it (ADR-0003).
struct SymbolKey {
  std::string project;   ///< stable project id
  std::string language;  ///< "python", "typescript", ...
  std::string path;      ///< project-relative, '/' separators, NFC
  std::string fqn;       ///< "pkg.mod.Class.method"
  SymbolKind kind = SymbolKind::function;
  std::uint32_t overload = 0;  ///< 0 unless (path, fqn, kind) repeats in a file
};

/// Stable identifier for a symbol: hex SHA-256 over the canonical encoding of
/// `key`. Same key in any process, on any platform, in any build gives the
/// same id.
std::string symbol_id(const SymbolKey& key);

/// Fingerprint of a symbol's body, used to decide whether a change to a file
/// actually changed this symbol. Normalises line endings and trailing
/// whitespace so that reformatting alone does not trigger revalidation.
///
/// Comment stripping is language-specific and is applied by the parser before
/// this is called; this function makes no attempt at it.
std::string body_fingerprint(std::string_view body);

/// Fingerprint of a byte sequence, for file contents and embedding chunks.
std::string content_fingerprint(std::string_view bytes);

}  // namespace veylqora
