#pragma once
#include <stdexcept>

namespace ct {
class TimeoutError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class ConnectionError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
} // namespace ct
