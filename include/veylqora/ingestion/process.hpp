#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace veylqora {

struct ProcessResult {
  int exit_code;
  std::string output;
};

/// Runs `git` in `dir` with the given arguments and captures stdout.
///
/// No shell is involved: arguments are passed as an argv array, so paths with
/// spaces or quotes cannot inject commands. `core.fsmonitor` is disabled, because
/// a repository's own config can name a program to run, and an indexed
/// repository is untrusted input (ADR-0009).
///
/// Returns nullopt if the process cannot be started (no git, or unsupported
/// platform). A started process always yields a result, even on failure.
std::optional<ProcessResult> run_git(const std::filesystem::path& dir,
                                     const std::vector<std::string>& args);

}  // namespace veylqora
