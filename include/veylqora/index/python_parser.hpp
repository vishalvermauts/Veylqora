#pragma once

#include "veylqora/index/source_model.hpp"

#include <string_view>

namespace veylqora {

/// Parses one Python module with tree-sitter. `module` is the dotted module
/// name derived from the file path, e.g. "pkg.util". For a package's
/// __init__.py, `is_package` is true and `module` names the package itself, so
/// relative imports inside it resolve against that package. Never executes code.
SourceFile parse_python(std::string_view module, std::string_view source,
                        bool is_package = false);

}  // namespace veylqora
