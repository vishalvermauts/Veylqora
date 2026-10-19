#include "veylqora/core/identity.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <string>

namespace veylqora {
namespace {

/// Unit separator. Chosen because it cannot occur in a path, a qualified name
/// or a language tag, so the canonical encoding is unambiguous without
/// escaping.
constexpr char kSep = '\x1f';

std::string sha256_hex(std::string_view bytes) {
  return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

/// Normalise a body for fingerprinting: CRLF and CR become LF, trailing
/// horizontal whitespace is dropped from every line, and leading and trailing
/// blank lines are removed. Interior blank lines are kept, because removing
/// them would merge genuinely different bodies.
std::string normalise_body(std::string_view body) {
  std::string out;
  out.reserve(body.size());

  std::string line;
  const auto flush_line = [&] {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    out += line;
    out += '\n';
    line.clear();
  };

  for (std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '\r') {
      if (i + 1 < body.size() && body[i + 1] == '\n') {
        ++i;
      }
      flush_line();
    } else if (c == '\n') {
      flush_line();
    } else {
      line += c;
    }
  }
  if (!line.empty()) {
    flush_line();
  }

  const auto first = out.find_first_not_of('\n');
  if (first == std::string::npos) {
    return {};
  }
  const auto last = out.find_last_not_of('\n');
  return out.substr(first, last - first + 1);
}

}  // namespace

std::string_view to_string(SymbolKind kind) noexcept {
  switch (kind) {
    case SymbolKind::file:
      return "file";
    case SymbolKind::module:
      return "module";
    case SymbolKind::klass:
      return "class";
    case SymbolKind::function:
      return "function";
    case SymbolKind::method:
      return "method";
    case SymbolKind::field:
      return "field";
  }
  return "unknown";
}

std::string symbol_id(const SymbolKey& key) {
  std::string canonical;
  canonical.reserve(key.project.size() + key.language.size() + key.path.size() +
                    key.fqn.size() + 32);
  canonical += key.project;
  canonical += kSep;
  canonical += key.language;
  canonical += kSep;
  canonical += key.path;
  canonical += kSep;
  canonical += key.fqn;
  canonical += kSep;
  canonical += to_string(key.kind);
  canonical += kSep;
  canonical += std::to_string(key.overload);
  return sha256_hex(canonical);
}

std::string body_fingerprint(std::string_view body) {
  return sha256_hex(normalise_body(body));
}

std::string content_fingerprint(std::string_view bytes) { return sha256_hex(bytes); }

}  // namespace veylqora
