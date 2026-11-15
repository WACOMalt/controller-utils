#include "sensor_clock.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ct {
std::optional<double> SensorClock::add(uint64_t tick, double received, uint64_t clock_modulus) {
    if ((clock_modulus != uint64_t(1) << 21 && clock_modulus != uint64_t(1) << 32) ||
        tick >= clock_modulus || !std::isfinite(received))
        throw std::invalid_argument("invalid clock sample");
    if (modulus && *modulus != clock_modulus)
        throw std::invalid_argument("sensor clock format changed; reacquire clock");
    modulus = clock_modulus;
    if (last_tick) {
        const auto delta = (tick + clock_modulus - *last_tick) % clock_modulus;
        if (!delta)
            return {};
        if (delta >= clock_modulus / 2 || delta > 2000000 || received - points.back().second > 1)
            throw std::invalid_argument("sensor clock reset or long input gap; reacquire clock");
        if (received <= points.back().second)
            throw std::invalid_argument("receive clock must increase");
        elapsed += delta;
    }
    last_tick = tick;
    points.emplace_back(elapsed * 1e-6, received);
    if (points.size() > 512)
        points.pop_front();
    std::vector<std::pair<double, double>> fit;
    double earliest = std::numeric_limits<double>::infinity();
    if (scale)
        for (const auto &[x, y] : points)
            earliest = std::min(earliest, y - (*scale * x + *offset));
    for (const auto &point : points)
        if (!scale || point.second - (*scale * point.first + *offset) <= earliest + .03)
            fit.push_back(point);
    if (fit.size() < 100 || fit.back().first - fit.front().first < .5) {
        timestamp =
            scale ? std::optional<double>(*scale * points.back().first + *offset) : std::nullopt;
        return timestamp;
    }
    long double sx = 0, sy = 0;
    for (const auto &[x, y] : fit) {
        sx += x;
        sy += y;
    }
    const double xmean = double(sx / fit.size()), ymean = double(sy / fit.size());
    long double xx = 0, xy = 0;
    for (const auto &[x, y] : fit) {
        xx += (x - xmean) * (x - xmean);
        xy += (x - xmean) * (y - ymean);
    }
    const double next_scale = double(xy / xx);
    if (!(next_scale > .8 && next_scale < 1.2))
        throw std::invalid_argument("sensor/host clock rate outside verified range");
    scale = next_scale;
    offset = ymean - *scale * xmean;
    double mapped = *scale * points.back().first + *offset;
    if (timestamp) {
        const double step = (points.back().first - points[points.size() - 2].first) * *scale;
        const double continuous =
            std::max(*timestamp + .75 * step, std::min(*timestamp + 1.25 * step, mapped));
        *offset += continuous - mapped;
        mapped = continuous;
    }
    timestamp = mapped;
    return timestamp;
}
} // namespace ct
