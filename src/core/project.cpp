#include "veylqora/core/project.hpp"

#include "veylqora/core/error.hpp"
#include "veylqora/core/identity.hpp"

namespace veylqora {

std::string project_id_for(const std::filesystem::path& root) {
  std::error_code ec;
  const std::filesystem::path canonical = std::filesystem::canonical(root, ec);
  if (ec) throw Error("cannot open " + root.string() + ": " + ec.message());
  return content_fingerprint(canonical.generic_string());
}

}  // namespace veylqora
