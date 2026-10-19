#include "veylqora/ingestion/source.hpp"

#include "veylqora/ingestion/process.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace veylqora {
namespace fs = std::filesystem;
namespace {

constexpr std::uintmax_t kMaxFileBytes = 1024 * 1024;

std::vector<std::string> split_nul(const std::string& text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\0', start);
    if (end == std::string::npos) break;
    if (end > start) out.emplace_back(text, start, end - start);
    start = end + 1;
  }
  return out;
}

bool in_git_work_tree(const fs::path& root) {
  const auto r = run_git(root, {"rev-parse", "--is-inside-work-tree"});
  return r && r->exit_code == 0 && r->output.starts_with("true");
}

void walk(const fs::path& root, std::vector<std::string>& out) {
  for (auto it = fs::recursive_directory_iterator(root);
       it != fs::recursive_directory_iterator(); ++it) {
    const fs::directory_entry& entry = *it;
    if (entry.is_symlink()) continue;
    if (entry.is_directory()) {
      if (entry.path().filename() == ".git") it.disable_recursion_pending();
      continue;
    }
    if (entry.is_regular_file())
      out.push_back(fs::relative(entry.path(), root).generic_string());
  }
}

}  // namespace

std::vector<std::string> list_candidates(const fs::path& root) {
  std::vector<std::string> out;
  if (in_git_work_tree(root)) {
    const auto files =
        run_git(root, {"ls-files", "-z", "--cached", "--others", "--exclude-standard"});
    if (files && files->exit_code == 0) {
      out = split_nul(files->output);
    }
  }
  if (out.empty()) walk(root, out);  // not git, git missing, or an empty repo
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool is_valid_utf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;  // allowed range for the first continuation byte
    if (c < 0x80) {
      ++i;
      continue;
    }
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      if (c == 0xE0) lo = 0xA0;  // reject overlong encodings
      if (c == 0xED) hi = 0x9F;  // reject UTF-16 surrogates
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      if (c == 0xF0) lo = 0x90;  // reject overlong encodings
      if (c == 0xF4) hi = 0x8F;  // reject code points above U+10FFFF
    } else {
      return false;
    }
    if (i + len > s.size()) return false;
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    if (b1 < lo || b1 > hi) return false;
    for (std::size_t k = 2; k < len; ++k) {
      const auto b = static_cast<unsigned char>(s[i + k]);
      if (b < 0x80 || b > 0xBF) return false;
    }
    i += len;
  }
  return true;
}

Source read_source(const fs::path& path) {
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(path, ec);
  if (ec || !fs::exists(st)) return {Skip::missing, {}};
  if (fs::is_symlink(st)) return {Skip::symlink, {}};
  if (!fs::is_regular_file(st)) return {Skip::not_regular, {}};

  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) return {Skip::unreadable, {}};
  if (size > kMaxFileBytes) return {Skip::too_large, {}};

  std::ifstream in(path, std::ios::binary);
  if (!in) return {Skip::unreadable, {}};
  std::ostringstream buf;
  buf << in.rdbuf();
  if (in.bad()) return {Skip::unreadable, {}};
  std::string contents = buf.str();

  if (contents.find('\0') != std::string::npos) return {Skip::binary, {}};
  if (!is_valid_utf8(contents)) return {Skip::invalid_utf8, {}};
  return {Skip::none, std::move(contents)};
}

}  // namespace veylqora
