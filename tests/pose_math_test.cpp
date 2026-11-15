#include "interaction/pose_math.h"
#include <cassert>
#include <iostream>

int main() {
    ct::Matrix pose = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1};
    const ct::Point point = {.1f, .2f, .3f};
    const auto restored = ct::transform(ct::inverse_rigid(pose), ct::transform(pose, point));
    for (int i = 0; i < 3; ++i)
        assert(std::abs(restored[i] - point[i]) < 1e-6f);
    const auto product = ct::multiply(ct::inverse_rigid(pose), pose), identity = ct::identity();
    for (int i = 0; i < 16; ++i)
        assert(std::abs(product[i] - identity[i]) < 1e-6f);
    for (float invalid : {0.f, 2.f, -1.f}) {
        auto bad = ct::identity();
        bad[0] = invalid;
        bool rejected = false;
        try {
            (void)ct::inverse_rigid(bad);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
    }
    for (float eye_x : {-.0325f, .0325f}) {
        const ct::Point eye{eye_x, .002f, .001f}, point{.05f, -.1f, -.45f};
        const auto projection = ct::quad_projection(1.6f, 1.6f, .6f, eye);
        float clip[4]{};
        const float local[4] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2], 1};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                clip[r] += projection[c * 4 + r] * local[c];
        const ct::Point panel_point{clip[0] / clip[3] * .8f, clip[1] / clip[3] * .8f, -.6f};
        for (int r = 0; r < 3; ++r) {
            const float ray_at_panel =
                eye[r] + (point[r] - eye[r]) * (-.6f - eye[2]) / (point[2] - eye[2]);
            assert(std::abs(panel_point[r] - ray_at_panel) < 1e-5f);
        }
    }
    bool bad_quad = false;
    try {
        (void)ct::quad_projection(0, 1, 1, {0, 0, 0});
    } catch (const std::runtime_error &) {
        bad_quad = true;
    }
    assert(bad_quad);
    const std::array<ct::Point, 2> eyes{{{-.0325f, 0, 0}, {.0325f, 0, 0}}};
    const auto fitted = ct::fit_model_quad({.1f, -.2f, -.6f}, .1f, eyes);
    assert(fitted && fitted->width < .4f && fitted->width > .2f);
    for (const auto &eye : eyes)
        for (int x : {-1, 1})
            for (int y : {-1, 1})
                for (int z : {-1, 1}) {
                    const float scale = -.6f / (-.6f + z * .1f);
                    const float projected_x = eye[0] + (.1f + x * .1f - eye[0]) * scale;
                    const float projected_y = (-.2f + y * .1f) * scale;
                    assert(std::abs(projected_x - fitted->center[0]) < fitted->width / 2);
                    assert(std::abs(projected_y - fitted->center[1]) < fitted->width / 2);
                    const auto projection = ct::quad_projection(
                        fitted->width, fitted->width, .6f,
                        {eye[0] - fitted->center[0], eye[1] - fitted->center[1], 0});
                    const float point[4] = {.1f + x * .1f - eye[0], -.2f + y * .1f, -.6f + z * .1f,
                                            1};
                    float clip[4]{};
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c)
                            clip[r] += projection[c * 4 + r] * point[c];
                    assert(std::abs(fitted->center[0] + clip[0] / clip[3] * fitted->width / 2 -
                                    projected_x) < 1e-5f);
                    assert(std::abs(fitted->center[1] + clip[1] / clip[3] * fitted->width / 2 -
                                    projected_y) < 1e-5f);
                }
    assert(!ct::fit_model_quad({0, 0, -.1f}, .1f, eyes));
    assert(ct::fit_model_quad({0, 0, -.3f}, .1f, eyes)->width > fitted->width);
    std::cout << "renderer pose math: rotation, translation, inverse, scale and reflection "
                 "rejection passed\n";
}
