#include "position.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ct {
namespace {
constexpr double CoastTime = .1, CoastDistance = .05;

Vector3 bounded_position(const Vector3 &point, const Vector3 &anchor) {
    const auto offset = subtract(point, anchor);
    const auto length = norm(offset);
    return length > CoastDistance ? add(anchor, scale(offset, CoastDistance / length)) : point;
}
} // namespace

void InertialPosition::anchor(double timestamp, const Vector3 &point) {
    position = coast_origin = point;
    velocity = Vector3{};
    observations = {{timestamp, point}};
    history = {{std::min(timestamp, *time), {point, {}}}};
    if (timestamp < *time)
        history.push_back({*time, {point, {}}});
}

void InertialPosition::interrupt() {
    time.reset();
    history.clear();
    acceleration = {};
    while (observations.size() > 1)
        observations.pop_front();
    stationary = false;
    stationary_anchor.reset();
    relocation.reset();
    if (velocity)
        velocity = Vector3{};
}

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
    if (angular_speed >= .12 || distance(force, bias) >= .35) {
        stationary = false;
        stationary_anchor.reset();
        if (relocation && relocation->palms)
            relocation.reset();
    }
    const double coast =
        observations.empty() ? 0 : std::clamp(observations.back().time + CoastTime - *time, 0., dt);
    time = timestamp;
    if (!observations.empty() && timestamp - observations.back().time > .25)
        stationary = false;
    const bool quiet = stationary_anchor && angular_speed < .07 && distance(force, bias) < .4;
    if (quiet && stationary)
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
    Vector3 next = add(*velocity, scale(applied, coast));
    next = scale(next, 1 / std::max(1., distance(next, {}) / 2));
    const auto next_position = add(*position, scale(add(*velocity, next), coast / 2));
    position = coast > 0 ? bounded_position(next_position, coast_origin) : next_position;
    velocity = timestamp < observations.back().time + CoastTime && *position == next_position
                   ? next
                   : Vector3{};
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
    const auto &last = observations.back();
    const auto sample = time.value_or(now);
    const double dt = std::clamp(std::min(now, last.time + CoastTime) - sample, 0., .03);
    auto result =
        dt > 0 ? bounded_position(add(*position, scale(*velocity, dt)), coast_origin) : *position;
    if (blend) {
        const double remaining =
            std::max(0., 1 - std::max(0., now - blend->started) / blend->duration);
        result = add(result, scale(blend->offset, remaining));
    }
    return result;
}
void InertialPosition::observe_grip(size_t side, double timestamp,
                                    const std::optional<Vector3> &center) {
    if (!stationary_anchor || !relocation || !relocation->palms || !std::isfinite(timestamp) ||
        timestamp < relocation->first || timestamp > relocation->latest + .02)
        return;
    auto &palm = relocation->palms->at(side);
    if (timestamp <= palm.latest)
        return;
    if (!center || !finite(*center) || distance(*center, relocation->point) > .025 ||
        distance(*center, relocation->point) + .015 >= distance(*center, *stationary_anchor))
        palm.first = 0;
    else if (!palm.first || timestamp - palm.latest > .25)
        palm.first = timestamp;
    palm.latest = timestamp;
}
InertialPosition::Fix InertialPosition::observe(double timestamp, const Vector3 &point,
                                                double received, double confidence) {
    if (!std::isfinite(timestamp) || !std::isfinite(received) || !std::isfinite(confidence) ||
        !(received - timestamp >= -.02 && received - timestamp <= .15) ||
        !(confidence > 0 && confidence <= 1) || !finite(point) ||
        std::any_of(point.begin(), point.end(), [](double v) { return std::abs(v) >= 20; }))
        throw std::invalid_argument("invalid inertial position observation");
    if (!observations.empty() && timestamp <= observations.back().time)
        throw std::invalid_argument("inertial observations must increase");
    if (!time || timestamp > *time + .02)
        return Fix::Rejected;
    if (!position) {
        anchor(timestamp, point);
        return Fix::Accepted;
    }
    const double aligned_time = std::min(timestamp, *time);
    const auto past = at(aligned_time);
    if (!past)
        return Fix::Rejected;
    const double previous = observations.back().time;
    const bool returning = timestamp - previous > .15;
    const auto error = subtract(point, past->position);
    const bool parked = stationary_anchor && distance(point, *stationary_anchor) > .04;
    const auto old_display = value(received);
    if (parked || norm(error) > (returning ? CoastDistance : .25)) {
        if (relocation && timestamp <= relocation->latest)
            return Fix::Rejected;
        if (parked && confidence < 1) {
            relocation.reset();
            return Fix::Rejected;
        }
        if (!relocation || parked != relocation->palms.has_value() ||
            timestamp - relocation->latest > .06 ||
            distance(point, relocation->point) >
                (parked ? .015 : .03 + 2 * (timestamp - relocation->latest))) {
            relocation = Relocation{timestamp, timestamp, point, 1};
            if (parked)
                relocation->palms.emplace();
        } else {
            relocation->latest = timestamp;
            // A parked candidate must stay in one place throughout palm confirmation.
            if (!parked)
                relocation->point = point;
            ++relocation->count;
        }
        if (relocation->count < 3 || timestamp - relocation->first < .02 ||
            (parked && std::none_of(
                           relocation->palms->begin(), relocation->palms->end(),
                           [&](const auto &palm) {
                               return palm.first > 0 && palm.latest - palm.first >= .08 &&
                                      received - palm.latest <= .18;
                           }))) {
            return Fix::Rejected;
        }
        anchor(timestamp, point);
        stationary = parked;
        if (parked)
            stationary_anchor = point;
        else
            stationary_anchor.reset();
        relocation.reset();
        blend = Blend{received, subtract(*old_display, point), std::max(.15, smoothing)};
        return parked ? Fix::PalmRelocated : Fix::Accepted;
    }
    relocation.reset();
    if (returning)
        observations.clear();
    observations.push_back({timestamp, point});
    while (observations.size() > 1 &&
           (timestamp - observations.front().time > .3 || observations.size() > 96))
        observations.pop_front();
    const double span = timestamp - observations.front().time;
    stationary = span >= .22 && observations.size() >= 6 && distance(acceleration, {}) < .35 &&
                 std::all_of(
                     observations.begin(), observations.end(),
                     [&](const auto &entry) { return distance(entry.position, point) < .006; });
    if (stationary && !stationary_anchor)
        stationary_anchor = point;
    const double tau = smoothing * (stationary ? 2 : 1);
    const double gain = returning || tau == 0 ? 1 : 1 - std::exp(-(timestamp - previous) / tau);
    const auto correction = scale(error, gain * confidence);
    Vector3 velocity_correction{};
    if (confidence == 1 && span > .08) {
        double mean = 0;
        for (const auto &entry : observations)
            mean += entry.time;
        mean /= observations.size();
        double variance = 0;
        Vector3 measured{};
        for (const auto &entry : observations) {
            const double dt = entry.time - mean;
            variance += dt * dt;
            for (size_t k = 0; k < 3; ++k)
                measured[k] += dt * entry.position[k];
        }
        measured = scale(measured, 1 / variance);
        if (stationary_anchor)
            measured = {};
        if (distance(measured, {}) < 2)
            velocity_correction = scale(subtract(measured, past->velocity), .3);
    }
    for (auto &entry : history)
        if (entry.time >= aligned_time) {
            entry.state.position = add(add(entry.state.position, correction),
                                       scale(velocity_correction, entry.time - aligned_time));
            entry.state.velocity = add(entry.state.velocity, velocity_correction);
        }
    position = history.back().state.position;
    coast_origin = *position;
    velocity = history.back().state.velocity;
    const double remaining = blend ? blend->started + blend->duration - received : 0;
    blend.reset();
    const double duration = std::max({returning ? .15 : 0., smoothing, remaining});
    if (duration)
        blend = Blend{received, subtract(*old_display, *value(received)), duration};
    return Fix::Accepted;
}
} // namespace ct
