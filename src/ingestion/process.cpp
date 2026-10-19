#include "veylqora/ingestion/process.hpp"

#if defined(_WIN32)

namespace veylqora {

// Process spawning is POSIX-only in this build. Windows falls back to the
// directory walker, which is correct but does not honour .gitignore (ADR-0009).
std::optional<ProcessResult> run_git(const std::filesystem::path& /*dir*/,
                                     const std::vector<std::string>& /*args*/) {
  return std::nullopt;
}

}  // namespace veylqora

#else

#include <array>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace veylqora {

std::optional<ProcessResult> run_git(const std::filesystem::path& dir,
                                     const std::vector<std::string>& args) {
  std::vector<std::string> argv_store = {"git",
                                         "-C",
                                         dir.string(),
                                         "-c",
                                         "core.fsmonitor=false",
                                         "-c",
                                         "core.untrackedCache=false"};
  argv_store.insert(argv_store.end(), args.begin(), args.end());
  std::vector<char*> argv;
  argv.reserve(argv_store.size() + 1);
  for (std::string& s : argv_store) argv.push_back(s.data());
  argv.push_back(nullptr);

  int fds[2];
  if (pipe(fds) != 0) return std::nullopt;

  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return std::nullopt;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    close(fds[0]);
    close(fds[1]);
    execvp("git", argv.data());
    _exit(127);  // exec failed: git is not installed or not on PATH
  }

  close(fds[1]);
  std::string output;
  std::array<char, 4096> buf{};
  while (true) {
    const ssize_t n = read(fds[0], buf.data(), buf.size());
    if (n > 0) {
      output.append(buf.data(), static_cast<std::size_t>(n));
    } else if (n == 0 || errno != EINTR) {
      break;
    }
  }
  close(fds[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
  if (code == 127) return std::nullopt;  // exec failed in the child
  return ProcessResult{code, std::move(output)};
}

}  // namespace veylqora

#endif
