#pragma once

#include "veylqora/index/source_model.hpp"

#include <string_view>

namespace veylqora {

enum class JsDialect : std::uint8_t { javascript, typescript };

/// Parses one JavaScript or TypeScript module with tree-sitter (ADR-0019).
/// `module` is the dotted module name of the file, e.g. "src.api.routes" for
/// src/api/routes.ts, and an index file names its directory: src/api/index.ts
/// is "src.api". `directory` is the file's '/'-separated directory relative to
/// the project root ("" at the root), which relative imports resolve against.
/// Never executes code.
SourceFile parse_javascript(std::string_view module, std::string_view directory,
                            std::string_view source, JsDialect dialect);

}  // namespace veylqora
