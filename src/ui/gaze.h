#pragma once
#include <openvr.h>
#include "interaction/pose_math.h"

namespace ct {
struct GazeRay {
    Point origin, direction;
};

inline std::optional<GazeRay> gaze_ray(const vr::HmdVector2_t &ndc,
                                       const vr::HmdMatrix44_t &projection,
                                       const Matrix &world_from_eye) {
    for (const auto &row : projection.m)
        for (float value : row)
            if (!std::isfinite(value))
                return {};
    for (float value : ndc.v)
        if (!std::isfinite(value) || std::abs(value) > 4)
            return {};
    const auto &m = projection.m;
    if (std::abs(m[0][0]) < 1e-5f || std::abs(m[1][1]) < 1e-5f ||
        std::abs(m[0][1]) + std::abs(m[1][0]) + std::abs(m[3][0]) + std::abs(m[3][1]) > 1e-5f)
        return {};
    const float w = m[3][3] - m[3][2];
    if (w <= 0)
        return {};
    const auto origin = transform(world_from_eye, {0, 0, 0});
    const auto target =
        transform(world_from_eye, {(ndc.v[0] * w + m[0][2] - m[0][3]) / m[0][0],
                                   (ndc.v[1] * w + m[1][2] - m[1][3]) / m[1][1], -1});
    Point direction{};
    float norm = 0;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(origin[i]) || !std::isfinite(target[i]))
            return {};
        direction[i] = target[i] - origin[i];
        norm += direction[i] * direction[i];
    }
    if (!std::isfinite(norm) || norm < 1e-8f)
        return {};
    for (auto &value : direction)
        value /= std::sqrt(norm);
    return GazeRay{origin, direction};
}
} // namespace ct
