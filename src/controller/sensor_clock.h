#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace ct {
class SensorClock {
  public:
    std::deque<std::pair<double, double>> points;
    std::optional<uint64_t> last_tick, modulus;
    uint64_t elapsed = 0;
    std::optional<double> scale, offset, timestamp;
    std::optional<double> add(uint64_t tick, double received,
                              uint64_t clock_modulus = uint64_t(1) << 32);
};
} // namespace ct
