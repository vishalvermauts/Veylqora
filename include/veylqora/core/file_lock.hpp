#pragma once

#include <cstdint>
#include <filesystem>

namespace veylqora {

/// Exclusive, non-blocking lock on a file, held for the object's lifetime.
/// The daemon holds `<db>.lock` so that a CLI in direct mode cannot write the
/// same database alongside it (ADR-0004).
class FileLock {
 public:
  /// Throws Error if another process holds the lock.
  explicit FileLock(const std::filesystem::path& file);
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  std::intptr_t fd_ = -1;
};

}  // namespace veylqora
