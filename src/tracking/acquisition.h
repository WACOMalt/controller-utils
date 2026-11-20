#pragma once
#include <cstdint>
#include <optional>

namespace ct {
enum class AlignmentAction { None, Heading, Restart };

class AlignmentGuide {
  public:
    std::optional<double> started, completed, hold_started, last_input;
    bool hold_fired = false, restart_fired = false;
    void begin(double now);
    void finish(double now);
    AlignmentAction update(uint32_t buttons, std::optional<double> received, double now);
    std::optional<double> progress(double now) const;
};

} // namespace ct
