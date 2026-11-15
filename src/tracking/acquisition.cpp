#include "acquisition.h"
#include <algorithm>
#include <cmath>

namespace ct {
bool AcquisitionQuality::ready() const {
    if (!time || !std::isfinite(*time))
        return false;
    for (const auto &slot : slots) {
        if (!slot.connected || slot.led_count != 1 || slot.seed_status ||
            (slot.seed_side != 0 && slot.seed_side != 1) || !slot.seed_time ||
            !std::isfinite(*slot.seed_time) || *time - *slot.seed_time < 0 ||
            *time - *slot.seed_time > .3)
            continue;
        const auto palm = std::find_if(palms.begin(), palms.end(),
                                       [&](const Palm &p) { return p.side == slot.seed_side; });
        if (palm == palms.end() || !palm->accepted || palm->covariance.size() != 6 ||
            !std::all_of(palm->covariance.begin(), palm->covariance.end(),
                         [](double value) { return std::isfinite(value) && value >= 0; }))
            continue;
        const Stereo *stereo = nullptr;
        bool duplicate = false;
        for (const auto &group : groups) {
            if (group.slot != slot.index)
                continue;
            if (stereo) {
                duplicate = true;
                break;
            }
            stereo = &group;
        }
        if (!stereo || duplicate || stereo->cameras.size() != 2)
            continue;
        if (std::all_of(stereo->cameras.begin(), stereo->cameras.end(), [](const Camera &camera) {
                return camera.count == 6 && camera.finite_count == 6 && camera.rms &&
                       std::isfinite(*camera.rms) && *camera.rms >= 0 && *camera.rms <= 30;
            }))
            return true;
    }
    return false;
}
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
        hold_started.reset();
        hold_fired = restart_fired = false;
    } else if (!hold_started)
        hold_started = now;
    else if (!restart_fired && now - *hold_started >= 5) {
        hold_fired = restart_fired = true;
        return AlignmentAction::Restart;
    } else if (!hold_fired && now - *hold_started >= 1) {
        hold_fired = true;
        return AlignmentAction::Heading;
    }
    return AlignmentAction::None;
}
std::optional<double> AlignmentGuide::progress(double now) const {
    if (!hold_started || restart_fired || !last_input || now - *last_input > .15)
        return {};
    const double elapsed = now - *hold_started;
    return std::clamp(hold_fired ? (elapsed - 1) / 4 : elapsed, 0., 1.);
}
bool InitialAcquisition::update(double timestamp, bool good) {
    if (ready)
        return true;
    if (!std::isfinite(timestamp) || (latest && timestamp <= *latest))
        return false;
    if (latest && timestamp - *latest > 1)
        since.reset();
    latest = timestamp;
    if (!good)
        since.reset();
    else if (!since)
        since = timestamp;
    else if (timestamp - *since >= .5)
        ready = true;
    return ready;
}
} // namespace ct
