#pragma once
#include "interaction/pointer_math.h"

namespace ct {
// Move only the native Pointer action pose. The tracked device's real pose stays
// available to the compositor for spatial window dragging.
struct TipCursor {
    Matrix anchor = identity();
    std::array<float, 2> first{}, angle{};
    PadMotionFilter motion;
    bool touching = false, active = false;

    static Matrix offset() {
        auto result = identity();
        result[13] = -.001f;
        result[14] = -.006f;
        return result;
    }
    void reset() {
        touching = active = false;
        angle = {};
        motion.reset();
    }
    Matrix update(const Matrix &body, float x, float y, double time, bool touch,
                  bool held = false) {
        if (!touch) {
            touching = false;
            motion.reset();
            if (!active || !held) {
                reset();
                return offset();
            }
        } else {
            if (!touching) {
                first = {x, y};
                touching = true;
            }
            if (!active && std::hypot(x - first[0], y - first[1]) > .045f) {
                anchor = multiply(body, offset());
                angle = {};
                active = true;
                motion.reset();
            }
        }
        if (!active)
            return offset();
        const auto delta = motion.update(x, y, time, touch);
        for (int i = 0; i < 2; ++i)
            angle[i] = std::clamp(angle[i] + delta[i] * .7f, -1.2f, 1.2f);
        const float cy = std::cos(angle[0]), sy = std::sin(angle[0]);
        const float cp = std::cos(angle[1]), sp = std::sin(angle[1]);
        // Positive pad X/Y turns the fixed world ray right/up. Rotation is
        // bounded relative to the captured ray, so swipes cannot wrap behind it.
        const Matrix turn{cy,       0,   sy,      0, -sy * sp, cp, cy * sp, 0,
                          -sy * cp, -sp, cy * cp, 0, 0,        0,  0,       1};
        return multiply(inverse_rigid(body), multiply(anchor, turn));
    }
};
} // namespace ct
