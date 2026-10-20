#include "veylqora/index/javascript_parser.hpp"

#include "tree_sitter_common.hpp"
#include "veylqora/core/error.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

extern "C" const TSLanguage* tree_sitter_javascript(void);
extern "C" const TSLanguage* tree_sitter_typescript(void);

namespace veylqora {
namespace {

using namespace ts;

struct Scope {
  std::size_t symbol;
  bool is_class;
  std::string fqn;
};

constexpr std::string_view kExtensions[] = {".tsx", ".mts", ".cts", ".jsx", ".mjs",
                                            ".cjs", ".ts",  ".js"};

std::vector<std::string> split_path(std::string_view path) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t stop = slash == std::string_view::npos ? path.size() : slash;
    parts.emplace_back(path.substr(start, stop - start));
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return parts;
}

// Turns an import specifier into the dotted module it names. "./x" and "../x"
// are resolved against the importing file's directory; anything else is a bare
// package name and keeps its own spelling. A trailing extension is dropped, and
// an "index" file names its directory, as the module of src/api/index.ts is
// "src.api".
std::string module_of_specifier(std::string_view directory, std::string_view spec) {
  const bool relative = spec.starts_with("./") || spec.starts_with("../");
  std::vector<std::string> parts;
  if (relative) parts = split_path(directory);
  if (parts.size() == 1 && parts[0].empty()) parts.clear();
  for (const std::string& seg : split_path(spec)) {
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(seg);
  }
  if (!parts.empty()) {
    for (const std::string_view ext : kExtensions) {
      if (parts.back().size() > ext.size() && parts.back().ends_with(ext)) {
        parts.back().resize(parts.back().size() - ext.size());
        break;
      }
    }
    if (parts.back() == "index" && parts.size() > 1) parts.pop_back();
  }
  std::string out;
  for (const std::string& seg : parts) out = join(out, seg);
  return out;
}

// The text of a string literal without its quotes.
std::string string_value(std::string_view source, TSNode literal) {
  const std::string_view text = text_of(source, literal);
  if (text.size() < 2) return {};
  return std::string(text.substr(1, text.size() - 2));
}

class Walker {
 public:
  Walker(std::string_view module, std::string_view directory, std::string_view source,
         SourceFile& out)
      : module_(module), directory_(directory), source_(source), out_(out) {}

  void run(TSNode root) {
    stack_.push_back(Scope{0, false, std::string(module_)});
    visit_children(root, 0);
  }

 private:
  std::size_t add_symbol(SymbolKind kind, std::string_view name, const std::string& fqn,
                         TSNode node) {
    const std::uint32_t overload =
        overloads_[fqn + '\x1f' + std::string(to_string(kind))]++;
    out_.symbols.push_back(SourceSymbol{std::string(name), fqn, kind, line_of(node),
                                        end_line_of(node),
                                        without_cr(code_text(source_, node)), overload});
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
    if (type == "function_declaration" || type == "generator_function_declaration") {
      return define(node, field(node, "name"), depth);
    }
    if (type == "class_declaration" || type == "abstract_class_declaration") {
      return define_class(node, depth);
    }
    if (type == "method_definition") return define(node, field(node, "name"), depth);
    if (type == "variable_declarator") return define_variable(node, depth);
    if (type == "call_expression") record_call(node);
    if (type == "new_expression") record_new(node);
    if (type == "import_statement") record_import(node);
    if (type == "export_statement") record_reexport(node);
    visit_children(node, depth);
  }

  // A function or method. Its kind follows the enclosing scope, as in Python.
  void define(TSNode node, TSNode name_node, int depth) {
    if (ts_node_is_null(name_node)) return visit_children(node, depth);
    const std::string_view name = text_of(source_, name_node);
    const bool parent_is_class = stack_.back().is_class;
    const std::string fqn = join(stack_.back().fqn, name);
    const SymbolKind kind = parent_is_class ? SymbolKind::method : SymbolKind::function;
    const std::size_t idx = add_symbol(kind, name, fqn, node);
    stack_.push_back(Scope{idx, false, fqn});
    visit_children(node, depth);
    stack_.pop_back();
  }

  void define_class(TSNode node, int depth) {
    const TSNode name_node = field(node, "name");
    if (ts_node_is_null(name_node)) return visit_children(node, depth);
    const std::string_view name = text_of(source_, name_node);
    const std::string fqn = join(stack_.back().fqn, name);
    const std::size_t idx = add_symbol(SymbolKind::klass, name, fqn, node);
    record_bases(node, idx, line_of(node));
    stack_.push_back(Scope{idx, true, fqn});
    visit_children(node, depth);
    stack_.pop_back();
  }

  // `const f = () => ...` and `const f = function () {...}` define a function
  // named by the variable. Other declarators are plain data and not symbols.
  void define_variable(TSNode node, int depth) {
    const TSNode value = field(node, "value");
    const TSNode name_node = field(node, "name");
    if (ts_node_is_null(value) || ts_node_is_null(name_node)) {
      return visit_children(node, depth);
    }
    const std::string_view value_type = ts_node_type(value);
    if (value_type != "arrow_function" && value_type != "function_expression" &&
        value_type != "function") {
      return visit_children(node, depth);
    }
    if (std::string_view(ts_node_type(name_node)) != "identifier") {
      return visit_children(node, depth);
    }
    const std::string_view name = text_of(source_, name_node);
    const std::string fqn = join(stack_.back().fqn, name);
    const SymbolKind kind =
        stack_.back().is_class ? SymbolKind::method : SymbolKind::function;
    const TSNode statement = ts_node_parent(node);
    const std::size_t idx = add_symbol(kind, name, fqn, statement);
    stack_.push_back(Scope{idx, false, fqn});
    visit_children(node, depth);
    stack_.pop_back();
  }

  // `class A extends B` and `extends ns.B`. TypeScript wraps the base in an
  // extends_clause; JavaScript puts the expression directly in class_heritage.
  void record_bases(TSNode node, std::size_t cls, std::size_t line) {
    const TSNode heritage = field_child(node, "class_heritage");
    if (ts_node_is_null(heritage)) return;
    const std::uint32_t n = ts_node_named_child_count(heritage);
    for (std::uint32_t i = 0; i < n; ++i) {
      TSNode base = ts_node_named_child(heritage, i);
      if (std::string_view(ts_node_type(base)) == "extends_clause") {
        if (ts_node_named_child_count(base) == 0) continue;
        base = ts_node_named_child(base, 0);
      }
      const std::string_view type = ts_node_type(base);
      if (type == "identifier") {
        out_.facts.push_back(SourceFact{FactKind::inherits, cls,
                                        std::string(text_of(source_, base)), {}, {}, line});
      } else if (type == "member_expression") {
        out_.facts.push_back(
            SourceFact{FactKind::inherits,
                       cls,
                       std::string(text_of(source_, field(base, "property"))),
                       receiver_text(field(base, "object")),
                       {},
                       line});
      }
    }
  }

  void record_call(TSNode node) {
    const TSNode fn = field(node, "function");
    if (ts_node_is_null(fn)) return;
    add_callee(fn, node);
  }

  // `new Thing()` is a call of Thing's constructor.
  void record_new(TSNode node) {
    const TSNode ctor = field(node, "constructor");
    if (ts_node_is_null(ctor)) return;
    add_callee(ctor, node);
  }

  void add_callee(TSNode callee, TSNode site) {
    const std::string_view type = ts_node_type(callee);
    std::string name;
    std::string qualifier;
    if (type == "identifier") {
      name = std::string(text_of(source_, callee));
    } else if (type == "member_expression") {
      name = std::string(text_of(source_, field(callee, "property")));
      qualifier = receiver_text(field(callee, "object"));
    } else {
      return;  // computed members, super() and other dynamic callees are not modelled
    }

    std::string scope;
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
      if (it->is_class) {
        scope = it->fqn;
        break;
      }
    }
    out_.facts.push_back(SourceFact{FactKind::call, stack_.back().symbol, std::move(name),
                                    std::move(qualifier), std::move(scope),
                                    line_of(site)});
  }

  // `this` plays the role of Python's `self`: the resolver reads both as "the
  // enclosing class".
  std::string receiver_text(TSNode object) const {
    if (ts_node_is_null(object)) return {};
    if (std::string_view(ts_node_type(object)) == "this") return "self";
    return std::string(text_of(source_, object));
  }

  // import x from "m"; import {a, b as c} from "m"; import * as ns from "m".
  void record_import(TSNode node) {
    const TSNode source_node = field(node, "source");
    if (ts_node_is_null(source_node)) return;
    const std::string mod =
        module_of_specifier(directory_, string_value(source_, source_node));
    add_import_fact(mod, line_of(node));

    const std::uint32_t n = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < n; ++i) {
      const TSNode clause = ts_node_named_child(node, i);
      if (std::string_view(ts_node_type(clause)) != "import_clause") continue;
      bind_clause(clause, mod);
    }
  }

  void bind_clause(TSNode clause, const std::string& mod) {
    const std::uint32_t n = ts_node_named_child_count(clause);
    for (std::uint32_t i = 0; i < n; ++i) {
      const TSNode part = ts_node_named_child(clause, i);
      const std::string_view type = ts_node_type(part);
      if (type == "identifier") {
        // A default import binds the module's default export.
        out_.bindings.push_back(
            SourceBinding{std::string(text_of(source_, part)), join(mod, "default")});
      } else if (type == "namespace_import") {
        const TSNode alias = ts_node_named_child(part, 0);
        if (!ts_node_is_null(alias)) {
          out_.bindings.push_back(
              SourceBinding{std::string(text_of(source_, alias)), mod});
        }
      } else if (type == "named_imports") {
        bind_specifiers(part, mod, "import_specifier");
      }
    }
  }

  // Each specifier is `name` or `name as alias`.
  void bind_specifiers(TSNode list, const std::string& mod, const char* kind) {
    const std::uint32_t n = ts_node_named_child_count(list);
    for (std::uint32_t i = 0; i < n; ++i) {
      const TSNode spec = ts_node_named_child(list, i);
      if (std::string_view(ts_node_type(spec)) != kind) continue;
      const std::string name = std::string(text_of(source_, specifier_name(spec)));
      const TSNode alias = specifier_alias(spec);
      const std::string local =
          ts_node_is_null(alias) ? name : std::string(text_of(source_, alias));
      out_.bindings.push_back(SourceBinding{local, join(mod, name)});
    }
  }

  TSNode specifier_name(TSNode spec) const {
    const TSNode name = field(spec, "name");
    return ts_node_is_null(name) ? ts_node_named_child(spec, 0) : name;
  }

  TSNode specifier_alias(TSNode spec) const {
    const TSNode alias = field(spec, "alias");
    if (!ts_node_is_null(alias)) return alias;
    return ts_node_named_child_count(spec) > 1 ? ts_node_named_child(spec, 1) : TSNode{};
  }

  // `export {a as b} from "m"` and `export * as ns from "m"` re-export names
  // from another module. A plain `export {x}` of a local name is not recorded:
  // the local binding table would overwrite the import it may have come from.
  void record_reexport(TSNode node) {
    const TSNode source_node = field(node, "source");
    if (ts_node_is_null(source_node)) return;
    const std::string mod =
        module_of_specifier(directory_, string_value(source_, source_node));
    add_import_fact(mod, line_of(node));

    const std::uint32_t n = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < n; ++i) {
      const TSNode part = ts_node_named_child(node, i);
      const std::string_view type = ts_node_type(part);
      if (type == "export_clause") {
        bind_specifiers(part, mod, "export_specifier");
      } else if (type == "namespace_export") {
        const TSNode alias = ts_node_named_child(part, 0);
        if (!ts_node_is_null(alias)) {
          out_.bindings.push_back(
              SourceBinding{std::string(text_of(source_, alias)), mod});
        }
      }
    }
  }

  void add_import_fact(const std::string& mod, std::size_t line) {
    out_.facts.push_back(
        SourceFact{FactKind::import, stack_.back().symbol, mod, {}, {}, line});
  }

  // A child found by node type rather than field name, for grammars that do not
  // name the class_heritage field.
  TSNode field_child(TSNode node, const char* type) const {
    const std::uint32_t n = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < n; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      if (std::string_view(ts_node_type(child)) == type) return child;
    }
    return TSNode{};
  }

  std::string_view module_;
  std::string_view directory_;
  std::string_view source_;
  SourceFile& out_;
  std::vector<Scope> stack_;
  std::unordered_map<std::string, std::uint32_t> overloads_;
};

}  // namespace

SourceFile parse_javascript(std::string_view module, std::string_view directory,
                            std::string_view source, JsDialect dialect) {
  if (source.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw Error("source too large to parse: " + std::string(module));
  }

  ParserPtr parser(ts_parser_new());
  const TSLanguage* language = dialect == JsDialect::typescript ? tree_sitter_typescript()
                                                                : tree_sitter_javascript();
  if (!ts_parser_set_language(parser.get(), language)) {
    throw Error("tree-sitter JavaScript/TypeScript grammar is incompatible with the runtime");
  }
  TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                      static_cast<std::uint32_t>(source.size())));
  if (!tree) throw Error("parse failed: " + std::string(module));

  const TSNode root = ts_tree_root_node(tree.get());
  SourceFile out;
  out.has_syntax_errors = ts_node_has_error(root);

  const std::size_t lines =
      static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n')) + 1;
  out.symbols.push_back(SourceSymbol{
      last_segment(module), std::string(module), SymbolKind::module, 1, lines, {}, 0});
  Walker(module, directory, source, out).run(root);
  return out;
}

}  // namespace veylqora
