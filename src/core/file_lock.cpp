#include "veylqora/core/file_lock.hpp"

#include "veylqora/core/error.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace veylqora {

FileLock::FileLock(const std::filesystem::path& file) {
  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    throw Error("cannot create lock file " + file.string());
  }
  OVERLAPPED ov{};
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
    CloseHandle(h);
    throw Error("the database is in use by another process (" + file.string() +
                "); stop the daemon or use --server");
  }
  fd_ = static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(h));
}

FileLock::~FileLock() {
  if (fd_ >= 0) {
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(fd_));
    OVERLAPPED ov{};
    UnlockFileEx(h, 0, 1, 0, &ov);
    CloseHandle(h);
  }
}

}  // namespace veylqora

#else

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace veylqora {

FileLock::FileLock(const std::filesystem::path& file) {
  const int fd = open(file.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0)
    throw Error("cannot create lock file " + file.string() + ": " + std::strerror(errno));
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    throw Error("the database is in use by another process (" + file.string() +
                "); stop the daemon or use --server");
  }
  fd_ = fd;
}

FileLock::~FileLock() {
  if (fd_ >= 0) close(static_cast<int>(fd_));
}

}  // namespace veylqora

#endif
