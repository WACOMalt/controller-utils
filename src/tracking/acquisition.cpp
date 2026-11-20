#include "acquisition.h"
#include <algorithm>
#include <cmath>

namespace ct {
void AlignmentGuide::begin(double now) {
    started = now;
    completed.reset();
}
void AlignmentGuide::finish(double now) {
    started.reset();
    completed = now;
}
AlignmentAction AlignmentGuide::update(uint32_t buttons, std::optional<double> received,
                                       double now) {
    if (!received || !(now - *received >= -.02 && now - *received <= .15)) {
        hold_started.reset();
        return AlignmentAction::None;
    }
    if (last_input && *received <= *last_input)
        return AlignmentAction::None;
    const bool gap = !last_input || *received - *last_input > .15;
    last_input = received;
    if ((buttons & 0x4040) != 0x4040 || gap) {
        const bool realign = !gap && !restart_fired && hold_started && now - *hold_started >= 1;
        hold_started.reset();
        hold_fired = restart_fired = false;
        return realign ? AlignmentAction::Heading : AlignmentAction::None;
    } else if (!hold_started)
        hold_started = now;
    else if (!restart_fired && now - *hold_started >= 5) {
        hold_fired = restart_fired = true;
        return AlignmentAction::Restart;
    } else if (!hold_fired && now - *hold_started >= 1)
        hold_fired = true;
    return AlignmentAction::None;
}
std::optional<double> AlignmentGuide::progress(double now) const {
    if (!hold_started || restart_fired || !last_input || now - *last_input > .15)
        return {};
    const double elapsed = now - *hold_started;
    return std::clamp(hold_fired ? (elapsed - 1) / 4 : elapsed, 0., 1.);
}
} // namespace ct
