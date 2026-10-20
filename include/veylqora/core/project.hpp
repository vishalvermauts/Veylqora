#pragma once

#include <filesystem>
#include <string>

namespace veylqora {

/// Stable id of the project rooted at `root`: a fingerprint of its canonical
/// path. Throws Error if the path does not exist.
std::string project_id_for(const std::filesystem::path& root);

}  // namespace veylqora
