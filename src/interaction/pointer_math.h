#pragma once
#include "pose_math.h"

namespace ct {
struct PadMotionFilter {
    static constexpr float quiet_radius = .006f;
    static constexpr double smoothing_seconds = .012;
    std::array<float, 2> accepted{}, filtered{};
    double time = 0;
    bool active = false;

    void reset() {
        active = false;
    }
    std::array<float, 2> update(float x, float y, double sample_time, bool touching) {
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1 || std::abs(y) > 1 ||
            !std::isfinite(sample_time) || sample_time < 0)
            throw std::runtime_error("invalid touchpad motion");
        if (active && sample_time <= time)
            return {};
        if (!touching) {
            reset();
            return {};
        }
        if (!active) {
            accepted = filtered = {x, y};
            time = sample_time;
            active = true;
            return {};
        }
        const float dx = x - accepted[0], dy = y - accepted[1];
        const float distance = std::hypot(dx, dy);
        if (distance > quiet_radius) {
            const float scale = 1 - quiet_radius / distance;
            accepted[0] += dx * scale;
            accepted[1] += dy * scale;
        }
        const float alpha = -std::expm1(-(sample_time - time) / smoothing_seconds);
        time = sample_time;
        std::array<float, 2> delta{};
        for (int i = 0; i < 2; ++i) {
            const float previous = filtered[i];
            filtered[i] += (accepted[i] - filtered[i]) * alpha;
            delta[i] = filtered[i] - previous;
        }
        return delta;
    }
};

} // namespace ct
