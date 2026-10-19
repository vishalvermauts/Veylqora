#pragma once

#include <stdexcept>

namespace veylqora {

/// An expected, user-visible failure: bad input, a missing path, an I/O error.
/// Programmer errors are not reported this way. C++20 has no std::expected, so
/// this exception is the error channel (ADR-0008).
class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

}  // namespace veylqora
