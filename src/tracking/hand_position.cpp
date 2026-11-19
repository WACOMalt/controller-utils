#include "hand_position.h"
#include <algorithm>
#include <cmath>

namespace ct {
void HandPosition::observe(double time, double received, const PalmPoints &palms,
                           const Quaternion &orientation, const std::optional<Vector3> &native,
                           const Vector3 &display, bool moving) {
    if (!std::isfinite(time) || time <= sample_ || !std::isfinite(received) || received < time ||
        received - time > .18 || (assisted_ && received < assisted_->received) ||
        !finite(orientation) || std::abs(dot(orientation, orientation) - 1) > .01 ||
        !finite(display))
        return;
    const double dt = time - sample_;
    if (dt > .28)
        grips_ = {};
    sample_ = time;
    if (drift_ && !drift_->released && grip_reference_) {
        for (size_t i = 0; i < palms.size(); ++i) {
            auto &since = drift_->palm_agreement[i];
            const auto &offset = grip_reference_->offsets[i];
            if (!native || !palms[i] || !offset || !finite(*palms[i]) || !finite(*native) ||
                distance(subtract(*palms[i], rotate(orientation, *offset)), *native) >= .035) {
                since.reset();
                continue;
            }
            if (!since || dt > .28)
                since = time;
            if (time - *since >= .08)
                drift_->released = received;
        }
    }
    PalmPoints centers;
    for (size_t i = 0; i < palms.size(); ++i) {
        auto &grip = grips_[i];
        if (!palms[i] || !finite(*palms[i])) {
            grip = {};
            continue;
        }
        const auto palm = *palms[i];
        if (!grip.learned) {
            if (!native || !finite(*native)) {
                grip = {};
                continue;
            }
            const auto offset = rotate(conjugate(orientation), subtract(palm, *native));
            if (norm(offset) > .16) {
                grip = {};
                continue;
            }
            if (!grip.offset || distance(offset, *grip.offset) > .018) {
                grip.offset = offset;
                grip.since = time;
                grip.enrollment_body = *native;
            }
            // Enroll only after the palm and native body have translated together.
            if (time - grip.since < .22 || distance(*native, grip.enrollment_body) < .04)
                continue;
            grip.learned = true;
            grip.agreed = time;
        }
        const auto center = subtract(palm, rotate(orientation, *grip.offset));
        if ((grip.previous && distance(center, *grip.previous) > .018 + 2.0 * dt) ||
            (!moving && grip.quiet_position && distance(center, *grip.quiet_position) > .018)) {
            grip = {};
            continue;
        }
        grip.previous = center;
        if (moving)
            grip.quiet_position.reset();
        else if (!grip.quiet_position)
            grip.quiet_position = center;
        if (native && distance(center, *native) < .025)
            grip.agreed = time;
        if (time - grip.agreed > 1.5) {
            grip = {};
            continue;
        }
        centers[i] = grip.quiet_position.value_or(center);
    }
    if (!centers[0] && !centers[1]) {
        agreement_since_.reset();
        return;
    }
    if (centers[0] && centers[1] && distance(*centers[0], *centers[1]) > .045) {
        grips_ = {};
        agreement_since_.reset();
        return;
    }
    const auto point = centers[0] && centers[1] ? scale(add(*centers[0], *centers[1]), .5)
                                                : (centers[0] ? *centers[0] : *centers[1]);
    GripReference reference{time, point, {}};
    for (size_t i = 0; i < centers.size(); ++i)
        if (centers[i])
            reference.offsets[i] = grips_[i].offset;
    grip_reference_ = reference;
    const bool following = assisted_ && !return_started_;
    const double separation = native ? distance(point, *native) : 0;
    if (native && !following && separation < .03)
        return;
    if (native && following && separation < .018) {
        if (!agreement_since_ || dt > .28)
            agreement_since_ = time;
        if (time - *agreement_since_ >= .12) {
            return_started_ = received;
            return;
        }
    } else {
        agreement_since_.reset();
    }
    // Start at the displayed position when a delayed palm measurement arrives.
    const auto delta = subtract(point, display);
    const double gain = 1 - std::exp(-std::min(dt, .12) / .03);
    const double max_step = std::min(.10, .015 + 1.3 * dt);
    const auto target =
        add(display, scale(delta, std::min(gain, max_step / std::max(norm(delta), 1e-9))));
    assisted_ = Correction{time, received, std::clamp(dt, .02, .085), display, target};
    return_started_.reset();
    drift_.reset();
}
std::optional<double> HandPosition::recent(double now) const {
    if (assisted_ && now >= assisted_->captured && now - assisted_->captured <= .15)
        return assisted_->captured;
    return {};
}
std::optional<Vector3> HandPosition::grip_center(size_t side, double stamp, const Vector3 &palm,
                                                 const Quaternion &orientation) const {
    if (!grip_reference_ || stamp < grip_reference_->captured ||
        stamp - grip_reference_->captured > 15)
        return {};
    const auto &offset = grip_reference_->offsets.at(side);
    if (!offset)
        return {};
    return subtract(palm, rotate(orientation, *offset));
}
void HandPosition::native_observed(double stamp, double received, const Vector3 &position,
                                   bool moving, bool palm_relocated) {
    if (!std::isfinite(stamp) || !std::isfinite(received) || !finite(position) ||
        stamp <= last_native_)
        return;
    if (!last_native_ || stamp - last_native_ > .15)
        native_resumed_ = stamp;
    if (!last_native_ || stamp - last_native_ > .075)
        native_since_ = stamp;
    last_native_ = stamp;
    if (palm_relocated) {
        if (drift_ && !drift_->released)
            drift_->released = received;
        if (assisted_ && !return_started_)
            return_started_ = received;
        return;
    }
    if (drift_ && !drift_->released) {
        if (moving || distance(position, drift_->stable_point) > .0025 ||
            drift_->stable_since < native_since_) {
            drift_->stable_point = position;
            drift_->stable_since = stamp;
        }
        if (!moving)
            drift_->last_rest = DriftLimit::Rest{stamp, position};
        const auto &rest = drift_->last_rest;
        const bool new_reach = moving && rest && rest->time >= native_since_ &&
                               stamp - rest->time < .35 && distance(position, rest->point) > .04;
        if (stamp - drift_->stable_since >= .60 || new_reach)
            drift_->released = received;
    }
    if (stamp - native_since_ < .05 || !grip_reference_)
        return;
    const auto &grip = *grip_reference_;
    if (assisted_ && !return_started_) {
        if (stamp <= assisted_->captured || received - assisted_->received < .30)
            return;
        return_started_ = received;
        // A continuous native stream can be extrapolating away from a verified grip.
        if (native_resumed_ <= assisted_->received && distance(position, assisted_->to) > .03)
            drift_ = DriftLimit{assisted_->to, position, stamp};
    } else if (!moving && stamp - grip.captured > .30 && stamp - grip.captured < 1. &&
               native_since_ <= grip.captured && distance(position, grip.center) < .03 &&
               (!drift_ || (drift_->released && grip.captured > *drift_->released))) {
        // The palm disappeared at rest before its position disagreed with the native estimate.
        drift_ = DriftLimit{position, position, stamp};
    }
}
Vector3 HandPosition::value(double now, const Vector3 &native) const {
    auto base = native;
    if (drift_ && (!drift_->released || now < *drift_->released + .15)) {
        // Pass small movements through and cap the offset if a real reach continues unseen.
        const auto delta = subtract(native, drift_->origin);
        const auto length = norm(delta);
        const auto discount = std::clamp(.75 * (length - .02), 0., .10);
        const auto weight =
            drift_->released ? std::clamp(1 - (now - *drift_->released) / .15, 0., 1.) : 1.;
        if (discount > 0)
            base = subtract(native, scale(delta, weight * discount / length));
    }
    if (!assisted_ || (return_started_ && now >= *return_started_ + .15))
        return base;
    const auto &hand = *assisted_;
    const double fraction = std::clamp((now - hand.received) / hand.duration, 0., 1.);
    const auto point = add(hand.from, scale(subtract(hand.to, hand.from), fraction));
    if (!return_started_)
        return point;
    const double blend = std::clamp((now - *return_started_) / .15, 0., 1.);
    return add(point, scale(subtract(base, point), blend));
}
} // namespace ct
