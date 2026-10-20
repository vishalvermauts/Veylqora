#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace veylqora {

using Timestamp = std::chrono::system_clock::time_point;

/// Time source. Injected so stored timestamps are reproducible in tests.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual Timestamp now() const = 0;
};

class SystemClock final : public Clock {
 public:
  Timestamp now() const override { return std::chrono::system_clock::now(); }
};

class FixedClock final : public Clock {
 public:
  explicit FixedClock(Timestamp t) : t_(t) {}
  Timestamp now() const override { return t_; }

 private:
  Timestamp t_;
};

/// Source of opaque identifiers for new rows.
class IdGenerator {
 public:
  virtual ~IdGenerator() = default;
  virtual std::string next() = 0;
};

/// 128 random bits as 32 lowercase hex characters.
class RandomIds final : public IdGenerator {
 public:
  std::string next() override;
};

/// "id-1", "id-2", ... for tests that need predictable ids.
class SequentialIds final : public IdGenerator {
 public:
  std::string next() override { return "id-" + std::to_string(++n_); }

 private:
  std::uint64_t n_ = 0;
};

/// UTC, second precision, e.g. "2023-11-14T22:13:20Z".
std::string to_iso8601(Timestamp t);

}  // namespace veylqora
