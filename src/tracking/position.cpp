#include "position.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace ct {
void InertialPosition::sample(double timestamp, const Vector3 &force, double angular_speed) {
    if (!std::isfinite(timestamp) || !finite(force) ||
        std::any_of(force.begin(), force.end(), [](double v) { return std::abs(v) >= 100; }) ||
        !std::isfinite(angular_speed) || angular_speed < 0)
        throw std::invalid_argument("invalid inertial position sample");
    if (!time) {
        time = timestamp;
        if (position) {
            history.push_back({timestamp, {*position, *velocity}});
            if (history.size() > 512)
                history.pop_front();
        }
        return;
    }
    const double dt = timestamp - *time;
    if (!(dt > 0 && dt <= .05))
        throw std::invalid_argument("inertial position sample gap");
    time = timestamp;
    if (!observations.empty() && timestamp - observations.back().time > .25)
        stationary = false;
    const bool quiet = stationary && angular_speed < .07 && distance(force, bias) < .4;
    if (quiet)
        bias = add(bias, scale(subtract(force, bias), 1 - std::exp(-dt / 1.5)));
    const double gain = 1 - std::exp(-dt / .02);
    acceleration = add(acceleration, scale(subtract(subtract(force, bias), acceleration), gain));
    if (!position)
        return;
    Vector3 applied = distance(acceleration, {}) >= .08 ? acceleration : Vector3{};
    if (quiet) {
        velocity = Vector3{};
        applied = {};
    }
    const double damping = timestamp - observations.back().time > .3 ? std::exp(-dt / 1.5) : 1;
    Vector3 next = scale(add(*velocity, scale(applied, dt)), damping);
    next = scale(next, 1 / std::max(1., distance(next, {}) / 2));
    position = add(*position, scale(add(*velocity, next), dt / 2));
    velocity = next;
    const auto &anchor = observations.back().position;
    const double separation = distance(*position, anchor);
    if (separation > .75) {
        position = add(anchor, scale(subtract(*position, anchor), .75 / separation));
        velocity = Vector3{};
    }
    history.push_back({timestamp, {*position, *velocity}});
    if (history.size() > 512)
        history.pop_front();
}
std::optional<InertialPosition::State> InertialPosition::at(double timestamp) const {
    for (size_t i = 1; i < history.size(); ++i) {
        const auto &first = history[i - 1], &second = history[i];
        if (timestamp < first.time || timestamp > second.time)
            continue;
        const double fraction = (timestamp - first.time) / (second.time - first.time);
        State result;
        for (size_t k = 0; k < 3; ++k) {
            result.position[k] = first.state.position[k] +
                                 fraction * (second.state.position[k] - first.state.position[k]);
            result.velocity[k] = first.state.velocity[k] +
                                 fraction * (second.state.velocity[k] - first.state.velocity[k]);
        }
        return result;
    }
    if (!history.empty() && std::abs(timestamp - history.back().time) < 1e-8)
        return history.back().state;
    return {};
}
std::optional<Vector3> InertialPosition::value(double now) const {
    if (!position)
        return {};
    const double dt = std::clamp(now - *time, 0., .03);
    auto result = add(*position, scale(*velocity, dt));
    if (blend) {
        const double remaining =
            std::max(0., 1 - std::max(0., now - blend->started) / blend->duration);
        result = add(result, scale(blend->offset, remaining));
    }
    return result;
}
bool InertialPosition::observe(double timestamp, const Vector3 &point, double received,
                               double confidence) {
    if (!std::isfinite(timestamp) || !std::isfinite(received) || !std::isfinite(confidence) ||
        !(received - timestamp >= -.02 && received - timestamp <= .15) ||
        !(confidence > 0 && confidence <= 1) || !finite(point) ||
        std::any_of(point.begin(), point.end(), [](double v) { return std::abs(v) >= 20; }))
        throw std::invalid_argument("invalid inertial position observation");
    if (!observations.empty() && timestamp <= observations.back().time)
        throw std::invalid_argument("inertial observations must increase");
    if (!time || timestamp > *time + .02)
        return false;
    if (!position) {
        position = point;
        velocity = Vector3{};
        observations.push_back({timestamp, point});
        history.push_back({*time, {*position, *velocity}});
        if (history.size() > 512)
            history.pop_front();
        return true;
    }
    const auto past = at(timestamp);
    if (!past)
        return false;
    const double previous = observations.back().time;
    const bool returning = timestamp - previous > .15;
    const auto error = subtract(point, past->position);
    if (distance(error, {}) > (returning ? 1.5 : .25))
        return false;
    const auto old_display = value(received);
    if (returning)
        observations.clear();
    observations.push_back({timestamp, point});
    if (observations.size() > 24)
        observations.pop_front();
    std::vector<const Observation *> window;
    for (const auto &entry : observations)
        if (timestamp - entry.time <= .3)
            window.push_back(&entry);
    const double span = window.back()->time - window.front()->time;
    stationary = span >= .22 && window.size() >= 6 && distance(acceleration, {}) < .35 &&
                 std::all_of(
                     window.begin(), window.end(),
                     [&](const auto *entry) { return distance(entry->position, point) < .006; });
    const double tau = smoothing * (stationary ? 2 : 1);
    const double gain = returning || tau == 0 ? 1 : 1 - std::exp(-(timestamp - previous) / tau);
    const auto correction = scale(error, gain * confidence);
    Vector3 velocity_correction{};
    if (confidence == 1 && span > .08) {
        double mean = 0;
        for (const auto *entry : window)
            mean += entry->time;
        mean /= window.size();
        double variance = 0;
        Vector3 measured{};
        for (const auto *entry : window) {
            const double dt = entry->time - mean;
            variance += dt * dt;
            for (size_t k = 0; k < 3; ++k)
                measured[k] += dt * entry->position[k];
        }
        measured = scale(measured, 1 / variance);
        if (stationary)
            measured = {};
        if (distance(measured, {}) < 2)
            velocity_correction = scale(subtract(measured, past->velocity), .3);
    }
    for (auto &entry : history)
        if (entry.time >= timestamp) {
            entry.state.position = add(add(entry.state.position, correction),
                                       scale(velocity_correction, entry.time - timestamp));
            entry.state.velocity = add(entry.state.velocity, velocity_correction);
        }
    position = history.back().state.position;
    velocity = history.back().state.velocity;
    blend.reset();
    const double duration = std::max(returning ? .15 : 0., smoothing);
    if (duration)
        blend = Blend{received, subtract(*old_display, *value(received)), duration};
    return true;
}
} // namespace ct
