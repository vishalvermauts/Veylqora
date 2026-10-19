#include "veylqora/index/python_symbols.hpp"

#include <optional>
#include <unordered_map>

namespace veylqora {
namespace {

struct Scope {
  std::size_t indent;
  std::size_t symbol;  // index into the output vector
  bool is_class;
};

struct Header {
  bool is_class;
  std::string_view name;
};

bool is_ident(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '_';
}

std::vector<std::string_view> split_lines(std::string_view source) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (true) {
    std::size_t end = source.find('\n', start);
    if (end == std::string_view::npos) end = source.size();
    std::string_view line = source.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    if (end == source.size()) break;
    start = end + 1;
  }
  return lines;
}

std::optional<Header> parse_header(std::string_view text) {
  bool is_class = false;
  if (text.starts_with("class ")) {
    is_class = true;
    text.remove_prefix(6);
  } else if (text.starts_with("async def ")) {
    text.remove_prefix(10);
  } else if (text.starts_with("def ")) {
    text.remove_prefix(4);
  } else {
    return std::nullopt;
  }
  std::size_t n = 0;
  while (n < text.size() && is_ident(text[n])) ++n;
  if (n == 0) return std::nullopt;
  return Header{is_class, text.substr(0, n)};
}

}  // namespace

std::vector<PySymbol> extract_python_symbols(std::string_view module,
                                             std::string_view source) {
  const auto lines = split_lines(source);
  std::vector<PySymbol> out;
  std::vector<Scope> stack;
  std::unordered_map<std::string, std::uint32_t> seen;
  std::size_t last_code = 0;  // 1-based number of the last code line processed

  // A scope ends at the last code line before the line that closed it, so
  // trailing blank lines and comments are not part of the body.
  const auto close_scope = [&](const Scope& s) {
    PySymbol& sym = out[s.symbol];
    sym.end_line = last_code;
    for (std::size_t l = sym.start_line; l <= sym.end_line; ++l) {
      sym.body += lines[l - 1];
      sym.body += '\n';
    }
  };

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    const std::size_t indent = line.find_first_not_of(" \t");
    if (indent == std::string_view::npos) continue;  // blank line
    const std::string_view text = line.substr(indent);
    if (text.front() == '#') continue;  // a comment neither opens nor closes a scope

    while (!stack.empty() && indent <= stack.back().indent) {
      close_scope(stack.back());
      stack.pop_back();
    }

    if (const auto header = parse_header(text)) {
      std::string fqn(module);
      for (const Scope& s : stack) fqn += "." + out[s.symbol].name;
      fqn += ".";
      fqn += header->name;

      const bool in_class = !stack.empty() && stack.back().is_class;
      const SymbolKind kind = header->is_class ? SymbolKind::klass
                              : in_class       ? SymbolKind::method
                                               : SymbolKind::function;
      const std::uint32_t overload = seen[fqn + '\x1f' + std::string(to_string(kind))]++;

      out.push_back(
          PySymbol{std::string(header->name), fqn, kind, i + 1, i + 1, {}, overload});
      stack.push_back(Scope{indent, out.size() - 1, header->is_class});
    }
    last_code = i + 1;
  }
  while (!stack.empty()) {
    close_scope(stack.back());
    stack.pop_back();
  }
  return out;
}

}  // namespace veylqora
