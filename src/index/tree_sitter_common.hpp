#pragma once

// Helpers shared by the tree-sitter walkers (ADR-0010, ADR-0019). Private to the
// index layer: nothing outside src/index includes this.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace veylqora::ts {

constexpr int kMaxDepth = 512;  // hostile nesting cannot exhaust the stack (ADR-0010)

struct ParserDeleter {
  void operator()(TSParser* p) const { ts_parser_delete(p); }
};
struct TreeDeleter {
  void operator()(TSTree* t) const { ts_tree_delete(t); }
};
using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr = std::unique_ptr<TSTree, TreeDeleter>;

inline std::string_view text_of(std::string_view source, TSNode node) {
  const std::uint32_t start = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_end_byte(node);
  return source.substr(start, end - start);
}

inline TSNode field(TSNode node, const char* name) {
  return ts_node_child_by_field_name(node, name,
                                     static_cast<std::uint32_t>(std::strlen(name)));
}

inline void collect_comments(TSNode node,
                             std::vector<std::pair<std::uint32_t, std::uint32_t>>& out) {
  if (std::string_view(ts_node_type(node)) == "comment") {
    out.emplace_back(ts_node_start_byte(node), ts_node_end_byte(node));
    return;
  }
  const std::uint32_t n = ts_node_child_count(node);
  for (std::uint32_t i = 0; i < n; ++i) collect_comments(ts_node_child(node, i), out);
}

// The text of a definition without its comments, so that editing a comment is
// not a change to the code (ADR-0014). A comment that fills its own line takes
// the line with it, so no blank line is left behind.
inline std::string code_text(std::string_view source, TSNode node) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> comments;
  collect_comments(node, comments);
  const std::uint32_t begin = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_end_byte(node);
  std::string out;
  std::uint32_t pos = begin;
  for (auto [start, stop] : comments) {
    std::uint32_t from = start;
    std::uint32_t to = stop;
    while (from > pos && (source[from - 1] == ' ' || source[from - 1] == '\t')) --from;
    const bool own_line = (from == begin || source[from - 1] == '\n') &&
                          to < source.size() && source[to] == '\n';
    if (own_line) {
      to += 1;  // take the newline too
    } else {
      from = start;
    }
    out += source.substr(pos, from - pos);
    pos = to;
  }
  out += source.substr(pos, end - pos);
  return out;
}

// Stored bodies use LF line endings, so one definition has one text on every
// platform. Carriage returns carry no meaning in source code.
inline std::string without_cr(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c != '\r') out += c;
  }
  return out;
}

inline std::size_t line_of(TSNode node) { return ts_node_start_point(node).row + 1; }

// The last line of a node. A node that ends exactly at column 0 ends on the
// previous line.
inline std::size_t end_line_of(TSNode node) {
  const TSPoint start = ts_node_start_point(node);
  TSPoint end = ts_node_end_point(node);
  if (end.column == 0 && end.row > start.row) --end.row;
  return end.row + 1;
}

inline std::string last_segment(std::string_view dotted) {
  const std::size_t dot = dotted.rfind('.');
  return std::string(dot == std::string_view::npos ? dotted : dotted.substr(dot + 1));
}

// The package a module belongs to: "pkg.sub.mod" -> "pkg.sub", "mod" -> "".
inline std::string package_of(std::string_view module) {
  const std::size_t dot = module.rfind('.');
  return dot == std::string_view::npos ? std::string{}
                                       : std::string(module.substr(0, dot));
}

inline std::string join(const std::string& a, std::string_view b) {
  if (a.empty()) return std::string(b);
  if (b.empty()) return a;
  return a + "." + std::string(b);
}

}  // namespace veylqora::ts
