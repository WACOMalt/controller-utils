#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace ct {
using Matrix = std::array<float, 16>; // Column-major, metres, right-handed OpenVR coordinates.
using Point = std::array<float, 3>;

inline Matrix identity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

inline Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix result{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                result[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return result;
}

inline Point transform(const Matrix &m, const Point &p) {
    Point result{};
    for (int r = 0; r < 3; ++r) {
        result[r] = m[12 + r];
        for (int k = 0; k < 3; ++k)
            result[r] += m[k * 4 + r] * p[k];
    }
    return result;
}

inline Matrix quad_projection(float width, float height, float distance, const Point &eye) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(distance) || width <= 0 ||
        height <= 0 || distance <= .05f)
        throw std::runtime_error("invalid stereo quad geometry");
    for (float value : eye)
        if (!std::isfinite(value))
            throw std::runtime_error("invalid eye position");
    const float d = distance + eye[2];
    if (d <= .02f)
        throw std::runtime_error("eye is behind the stereo quad");
    const float left = (-width / 2 - eye[0]) / d, right = (width / 2 - eye[0]) / d;
    const float bottom = (-height / 2 - eye[1]) / d, top = (height / 2 - eye[1]) / d;
    Matrix result{};
    result[0] = 2 / (right - left);
    result[5] = 2 / (top - bottom);
    result[8] = (right + left) / (right - left);
    result[9] = (top + bottom) / (top - bottom);
    result[10] = -1.004008f;
    result[11] = -1;
    result[14] = -.04008016f;
    return result;
}

struct QuadLayout {
    Point center;
    float width;
};

inline std::optional<QuadLayout> fit_model_quad(const Point &center, float radius,
                                                const std::array<Point, 2> &eyes) {
    for (float value : center)
        if (!std::isfinite(value))
            throw std::runtime_error("invalid model centre");
    if (!std::isfinite(radius) || radius <= 0 || radius > .3f)
        throw std::runtime_error("invalid model radius");
    if (center[2] + radius >= -.05f)
        return {};
    float left = 100, right = -100, bottom = 100, top = -100;
    for (const auto &eye : eyes) {
        for (float value : eye)
            if (!std::isfinite(value) || std::abs(value) > .2f)
                throw std::runtime_error("invalid eye offset");
        for (int x : {-1, 1})
            for (int y : {-1, 1})
                for (int z : {-1, 1}) {
                    const float scale = (-.6f - eye[2]) / (center[2] + z * radius - eye[2]);
                    const float px = eye[0] + (center[0] + x * radius - eye[0]) * scale;
                    const float py = eye[1] + (center[1] + y * radius - eye[1]) * scale;
                    left = std::min(left, px);
                    right = std::max(right, px);
                    bottom = std::min(bottom, py);
                    top = std::max(top, py);
                }
    }
    const float width = 1.08f * std::max(right - left, top - bottom);
    if (width > 3.3f)
        return {};
    return QuadLayout{{(left + right) / 2, (bottom + top) / 2, -.6f}, width};
}

inline Matrix inverse_rigid(const Matrix &m) {
    for (float x : m)
        if (!std::isfinite(x))
            throw std::runtime_error("nonfinite pose");
    if (std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]) + std::abs(m[15] - 1) > 1e-4f)
        throw std::runtime_error("pose must be affine");
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float dot = 0;
            for (int k = 0; k < 3; ++k)
                dot += m[i * 4 + k] * m[j * 4 + k];
            if (std::abs(dot - (i == j ? 1.f : 0.f)) > 1e-3f)
                throw std::runtime_error("pose must be rigid");
        }
    }
    const float determinant = m[0] * (m[5] * m[10] - m[9] * m[6]) -
                              m[4] * (m[1] * m[10] - m[9] * m[2]) +
                              m[8] * (m[1] * m[6] - m[5] * m[2]);
    if (determinant < .999f)
        throw std::runtime_error("pose must be right-handed");
    Matrix result = identity();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            result[c * 4 + r] = m[r * 4 + c];
        result[12 + r] = -(m[r * 4] * m[12] + m[r * 4 + 1] * m[13] + m[r * 4 + 2] * m[14]);
    }
    return result;
}
} // namespace ct
