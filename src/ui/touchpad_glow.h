#pragma once
#include "interaction/pointer_math.h"
#include <cstdint>

namespace ct::ui {
struct TouchpadGlow {
    static constexpr std::array<uint32_t, 2> masks{0x02000000, 0x00200000};
    std::array<PadMotionFilter, 2> motion{};
    std::array<std::array<float, 4>, 2> contacts{};
    double last_frame = 0;

    void update(uint32_t buttons, const float (&left)[3], const float (&right)[3],
                double input_time, double now, bool visible) {
        if (!visible || !std::isfinite(now) || !std::isfinite(input_time) || input_time <= 0 ||
            now - input_time > .15 || now - input_time < -.02) {
            for (auto &filter : motion)
                filter.reset();
            contacts = {};
            last_frame = 0;
            return;
        }
        const double dt = last_frame ? std::clamp(now - last_frame, 0., .05) : 1. / 90;
        last_frame = now;
        const float *pads[]{left, right};
        for (size_t side = 0; side < motion.size(); ++side) {
            const bool touching = buttons & masks[side];
            auto &filter = motion[side];
            auto &contact = contacts[side];
            if (touching && !filter.active)
                contact[2] = 0;
            filter.update(pads[side][0], pads[side][1], input_time, touching);
            if (touching) {
                contact[0] = filter.filtered[0];
                contact[1] = filter.filtered[1];
            }
            contact[2] = std::clamp(contact[2] + float(dt / (touching ? .03 : -.08)), 0.f, 1.f);
        }
    }
};
} // namespace ct::ui
