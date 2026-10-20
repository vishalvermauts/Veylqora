#include "veylqora/index/python_parser.hpp"

#include "veylqora/core/error.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <utility>

extern "C" const TSLanguage* tree_sitter_python(void);

namespace veylqora {
namespace {

constexpr int kMaxDepth = 512;  // hostile nesting cannot exhaust the stack (ADR-0010)

struct ParserDeleter {
  void operator()(TSParser* p) const { ts_parser_delete(p); }
};
struct TreeDeleter {
  void operator()(TSTree* t) const { ts_tree_delete(t); }
};
using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr = std::unique_ptr<TSTree, TreeDeleter>;

struct Scope {
  std::size_t symbol;
  bool is_class;
  std::string fqn;
};

std::string_view text_of(std::string_view source, TSNode node) {
  const std::uint32_t start = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_end_byte(node);
  return source.substr(start, end - start);
}

TSNode field(TSNode node, const char* name) {
  return ts_node_child_by_field_name(node, name,
                                     static_cast<std::uint32_t>(std::strlen(name)));
}

// Stored bodies use LF line endings, so one definition has one text on every
// platform. Carriage returns carry no meaning in Python source.
std::string without_cr(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c != '\r') out += c;
  }
  return out;
}

std::size_t line_of(TSNode node) { return ts_node_start_point(node).row + 1; }

// The last line of a node. A node that ends exactly at column 0 ends on the
// previous line.
std::size_t end_line_of(TSNode node) {
  const TSPoint start = ts_node_start_point(node);
  TSPoint end = ts_node_end_point(node);
  if (end.column == 0 && end.row > start.row) --end.row;
  return end.row + 1;
}

std::string last_segment(std::string_view dotted) {
  const std::size_t dot = dotted.rfind('.');
  return std::string(dot == std::string_view::npos ? dotted : dotted.substr(dot + 1));
}

// The package a module belongs to: "pkg.sub.mod" -> "pkg.sub", "mod" -> "".
std::string package_of(std::string_view module) {
  const std::size_t dot = module.rfind('.');
  return dot == std::string_view::npos ? std::string{}
                                       : std::string(module.substr(0, dot));
}

std::string join(const std::string& a, std::string_view b) {
  if (a.empty()) return std::string(b);
  if (b.empty()) return a;
  return a + "." + std::string(b);
}

class Walker {
 public:
  Walker(std::string_view module, std::string_view source, PyFile& out)
      : module_(module), source_(source), out_(out) {}

  void run(TSNode root) {
    stack_.push_back(Scope{0, false, std::string(module_)});
    visit_children(root, 0);
  }

 private:
  std::size_t add_symbol(SymbolKind kind, std::string_view name, const std::string& fqn,
                         TSNode node) {
    const std::uint32_t overload =
        overloads_[fqn + '\x1f' + std::string(to_string(kind))]++;
    out_.symbols.push_back(PySymbol{std::string(name), fqn, kind, line_of(node),
                                    end_line_of(node), without_cr(text_of(source_, node)),
                                    overload});
    return out_.symbols.size() - 1;
  }

  void visit_children(TSNode node, int depth) {
    const std::uint32_t n = ts_node_child_count(node);
    for (std::uint32_t i = 0; i < n; ++i) visit(ts_node_child(node, i), depth + 1);
  }

  void visit(TSNode node, int depth) {
    if (depth > kMaxDepth) {
      out_.has_syntax_errors = true;  // treated as partial, not as a crash
      return;
    }
    const std::string_view type = ts_node_type(node);
    if (type == "function_definition") return define(node, false, depth);
    if (type == "class_definition") return define(node, true, depth);
    if (type == "call") record_call(node);
    if (type == "import_statement") record_import(node);
    if (type == "import_from_statement") record_from_import(node);
    visit_children(node, depth);
  }

  void define(TSNode node, bool is_class, int depth) {
    const TSNode name_node = field(node, "name");
    if (ts_node_is_null(name_node)) return visit_children(node, depth);

    const std::string_view name = text_of(source_, name_node);
    const bool parent_is_class = stack_.back().is_class;
    const std::string fqn = join(stack_.back().fqn, name);
    const SymbolKind kind = is_class          ? SymbolKind::klass
                            : parent_is_class ? SymbolKind::method
                                              : SymbolKind::function;
    const std::size_t idx = add_symbol(kind, name, fqn, node);
    if (is_class) record_bases(node, idx, line_of(node));
    stack_.push_back(Scope{idx, is_class, fqn});
    visit_children(node, depth);
    stack_.pop_back();
  }

  void record_bases(TSNode node, std::size_t cls, std::size_t line) {
    const TSNode supers = field(node, "superclasses");
    if (ts_node_is_null(supers)) return;
    const std::uint32_t n = ts_node_named_child_count(supers);
    for (std::uint32_t i = 0; i < n; ++i) {
      const TSNode base = ts_node_named_child(supers, i);
      const std::string_view type = ts_node_type(base);
      if (type == "identifier") {
        out_.facts.push_back(PyFact{
            FactKind::inherits, cls, std::string(text_of(source_, base)), {}, {}, line});
      } else if (type == "attribute") {
        out_.facts.push_back(
            PyFact{FactKind::inherits,
                   cls,
                   std::string(text_of(source_, field(base, "attribute"))),
                   std::string(text_of(source_, field(base, "object"))),
                   {},
                   line});
      }
    }
  }

  void record_call(TSNode node) {
    const TSNode fn = field(node, "function");
    if (ts_node_is_null(fn)) return;
    const std::string_view type = ts_node_type(fn);

    std::string name;
    std::string qualifier;
    if (type == "identifier") {
      name = std::string(text_of(source_, fn));
    } else if (type == "attribute") {
      name = std::string(text_of(source_, field(fn, "attribute")));
      qualifier = std::string(text_of(source_, field(fn, "object")));
    } else {
      return;  // subscripts and other dynamic callees are not modelled
    }

    std::string scope;
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
      if (it->is_class) {
        scope = it->fqn;
        break;
      }
    }
    out_.facts.push_back(PyFact{FactKind::call, stack_.back().symbol, std::move(name),
                                std::move(qualifier), std::move(scope), line_of(node)});
  }

  void record_import(TSNode node) {
    const std::uint32_t n = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < n; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      const std::string_view type = ts_node_type(child);
      if (type == "dotted_name") {
        const std::string module(text_of(source_, child));
        // `import a.b` binds the first segment, `a`, to the top-level package.
        const std::string first = module.substr(0, module.find('.'));
        out_.bindings.push_back(PyBinding{first, first});
        add_import_fact(module, line_of(child));
      } else if (type == "aliased_import") {
        const std::string module(text_of(source_, field(child, "name")));
        const std::string alias(text_of(source_, field(child, "alias")));
        out_.bindings.push_back(PyBinding{alias, module});
        add_import_fact(module, line_of(child));
      }
    }
  }

  void record_from_import(TSNode node) {
    const TSNode mod = field(node, "module_name");
    if (ts_node_is_null(mod)) return;
    const std::string base = absolute_module(mod);
    add_import_fact(base, line_of(node));

    const std::uint32_t n = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < n; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      if (ts_node_start_byte(child) == ts_node_start_byte(mod))
        continue;  // the module itself
      const std::string_view type = ts_node_type(child);
      if (type == "dotted_name") {
        const std::string name(text_of(source_, child));
        out_.bindings.push_back(PyBinding{name, join(base, name)});
      } else if (type == "aliased_import") {
        const std::string name(text_of(source_, field(child, "name")));
        const std::string alias(text_of(source_, field(child, "alias")));
        out_.bindings.push_back(PyBinding{alias, join(base, name)});
      }
    }
  }

  // `from .x import y` inside pkg.mod means pkg.x. Each extra dot climbs one
  // package level from the current module's package.
  std::string absolute_module(TSNode mod) const {
    if (std::string_view(ts_node_type(mod)) == "dotted_name") {
      return std::string(text_of(source_, mod));
    }
    const TSNode prefix = ts_node_named_child(mod, 0);
    const std::size_t dots = text_of(source_, prefix).size();
    std::string base = package_of(module_);
    for (std::size_t d = 1; d < dots && !base.empty(); ++d) base = package_of(base);
    const TSNode rest =
        ts_node_named_child_count(mod) > 1 ? ts_node_named_child(mod, 1) : TSNode{};
    return join(base,
                ts_node_is_null(rest) ? std::string_view{} : text_of(source_, rest));
  }

  void add_import_fact(const std::string& module, std::size_t line) {
    out_.facts.push_back(
        PyFact{FactKind::import, stack_.back().symbol, module, {}, {}, line});
  }

  std::string_view module_;
  std::string_view source_;
  PyFile& out_;
  std::vector<Scope> stack_;
  std::unordered_map<std::string, std::uint32_t> overloads_;
};

}  // namespace

PyFile parse_python(std::string_view module, std::string_view source) {
  if (source.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw Error("source too large to parse: " + std::string(module));
  }

  ParserPtr parser(ts_parser_new());
  if (!ts_parser_set_language(parser.get(), tree_sitter_python())) {
    throw Error("tree-sitter Python grammar is incompatible with the runtime");
  }
  TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                      static_cast<std::uint32_t>(source.size())));
  if (!tree) throw Error("parse failed: " + std::string(module));

  const TSNode root = ts_tree_root_node(tree.get());
  PyFile out;
  out.has_syntax_errors = ts_node_has_error(root);

  const std::size_t lines =
      static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n')) + 1;
  out.symbols.push_back(PySymbol{
      last_segment(module), std::string(module), SymbolKind::module, 1, lines, {}, 0});
  Walker(module, source, out).run(root);
  return out;
}

}  // namespace veylqora
