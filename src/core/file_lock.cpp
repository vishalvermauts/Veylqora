#include "veylqora/core/file_lock.hpp"

#include "veylqora/core/error.hpp"

#if defined(_WIN32)

namespace veylqora {

FileLock::FileLock(const std::filesystem::path& /*file*/) {}
FileLock::~FileLock() = default;

}  // namespace veylqora

#else

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace veylqora {

FileLock::FileLock(const std::filesystem::path& file) {
  fd_ = open(file.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd_ < 0)
    throw Error("cannot create lock file " + file.string() + ": " + std::strerror(errno));
  if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    close(fd_);
    fd_ = -1;
    throw Error("the database is in use by another process (" + file.string() +
                "); stop the daemon or use --server");
  }
}

FileLock::~FileLock() {
  if (fd_ >= 0) close(fd_);
}

}  // namespace veylqora

#endif
