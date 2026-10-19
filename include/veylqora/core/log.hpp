#pragma once

#include <string_view>

namespace veylqora::log {

/// Writes a warning to stderr. Warnings report skipped input, not failures.
void warn(std::string_view message);

}  // namespace veylqora::log
