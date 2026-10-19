#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace veylqora {

/// Why a candidate file was not read for indexing.
enum class Skip : std::uint8_t {
  none,          // readable, indexable text
  missing,       // listed but gone from disk (tracked file deleted in the working tree)
  symlink,       // never followed: the target may be outside the project
  not_regular,   // directory, device, socket
  too_large,     // over the per-file limit
  binary,        // contains NUL bytes
  invalid_utf8,  // not valid UTF-8
  unreadable,    // I/O error while reading; previously indexed data is kept
};

struct Source {
  Skip skip = Skip::none;
  std::string contents;  // set only when skip == Skip::none
};

/// Repository-relative paths of every candidate file under `root`, sorted.
///
/// Inside a git work tree this is `git ls-files --cached --others
/// --exclude-standard`, which honours .gitignore, nested ignore files and
/// .git/info/exclude. Outside git, or where git is unavailable, it is a
/// recursive walk that skips `.git` directories (ADR-0009).
std::vector<std::string> list_candidates(const std::filesystem::path& root);

/// Reads one candidate and classifies it. Never follows a symlink.
Source read_source(const std::filesystem::path& path);

/// True if `bytes` is well-formed UTF-8 (RFC 3629: no overlongs, no surrogates,
/// nothing above U+10FFFF).
bool is_valid_utf8(std::string_view bytes);

}  // namespace veylqora
