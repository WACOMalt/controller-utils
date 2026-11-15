#include "ui/gaze.h"
#include <cassert>
#include <limits>
#include <cstdio>

int main() {
    vr::HmdMatrix44_t projection{};
    projection.m[0][0] = 1.4f;
    projection.m[1][1] = 1.7f;
    projection.m[0][2] = .18f;
    projection.m[1][2] = -.11f;
    projection.m[2][2] = -1.01f;
    projection.m[2][3] = -.1f;
    projection.m[3][2] = -1;
    auto eye = ct::identity();
    eye[12] = -.032f;
    for (float x : {-.4f, 0.f, .6f})
        for (float y : {-.5f, 0.f, .3f}) {
            vr::HmdVector2_t ndc{{projection.m[0][0] * x - projection.m[0][2],
                                  projection.m[1][1] * y - projection.m[1][2]}};
            const auto ray = ct::gaze_ray(ndc, projection, eye);
            assert(ray);
            const float length = std::sqrt(x * x + y * y + 1);
            assert(std::abs(ray->origin[0] + .032f) < 1e-6f);
            assert(std::abs(ray->direction[0] - x / length) < 1e-6f);
            assert(std::abs(ray->direction[1] - y / length) < 1e-6f);
            assert(std::abs(ray->direction[2] + 1 / length) < 1e-6f);
        }
    const ct::Matrix turned = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 2, 3, 1};
    auto ray = ct::gaze_ray({{-projection.m[0][2], -projection.m[1][2]}}, projection, turned);
    assert(ray && ray->origin == ct::Point({1, 2, 3}));
    assert(std::abs(ray->direction[0] + 1) < 1e-6f && std::abs(ray->direction[2]) < 1e-6f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    assert(!ct::gaze_ray({{nan, 0}}, projection, eye));
    assert(!ct::gaze_ray({{5, 0}}, projection, eye));
    auto bad = projection;
    bad.m[0][0] = 0;
    assert(!ct::gaze_ray({{0, 0}}, bad, eye));
    bad = projection;
    bad.m[2][3] = nan;
    assert(!ct::gaze_ray({{0, 0}}, bad, eye));
    bad = projection;
    bad.m[0][1] = .2f;
    assert(!ct::gaze_ray({{0, 0}}, bad, eye));
    bad = projection;
    bad.m[3][2] = 1;
    assert(!ct::gaze_ray({{0, 0}}, bad, eye));
    eye[12] = nan;
    assert(!ct::gaze_ray({{0, 0}}, projection, eye));
    std::puts(
        "gaze ray: asymmetric projections, eye origin, world rotation and invalid data passed");
}
