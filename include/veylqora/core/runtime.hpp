#pragma once

#include <chrono>
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

/// UTC, second precision, e.g. "2023-11-14T22:13:20Z".
std::string to_iso8601(Timestamp t);

}  // namespace veylqora
