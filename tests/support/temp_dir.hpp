#pragma once

#include <filesystem>
#include <random>
#include <string>

namespace veylqora::test {

/// A temporary directory removed on destruction.
class TempDir {
 public:
  TempDir()
      : path_(std::filesystem::temp_directory_path() /
              ("veylqora-test-" + std::to_string(std::random_device{}()))) {
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace veylqora::test
