#include "veylqora/core/log.hpp"

#include <iostream>

namespace veylqora::log {

void warn(std::string_view message) {
  std::cerr << "veylqora: warning: " << message << '\n';
}

}  // namespace veylqora::log
