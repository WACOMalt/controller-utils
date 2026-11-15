#include "math.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ct {
double dot(const Vector3 &a, const Vector3 &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
double dot(const Quaternion &a, const Quaternion &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}
double norm(const Vector3 &value) {
    return std::sqrt(dot(value, value));
}
double distance(const Vector3 &a, const Vector3 &b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
Vector3 cross(const Vector3 &a, const Vector3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vector3 add(const Vector3 &a, const Vector3 &b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Vector3 subtract(const Vector3 &a, const Vector3 &b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vector3 scale(const Vector3 &value, double factor) {
    return {value[0] * factor, value[1] * factor, value[2] * factor};
}
bool finite(const Vector3 &value) {
    return std::all_of(value.begin(), value.end(), [](double v) { return std::isfinite(v); });
}
bool finite(const Quaternion &value) {
    return std::all_of(value.begin(), value.end(), [](double v) { return std::isfinite(v); });
}
Quaternion multiply(const Quaternion &a, const Quaternion &b) {
    const auto [w, x, y, z] = a;
    const auto [v, i, j, k] = b;
    return {w * v - x * i - y * j - z * k, w * i + x * v + y * k - z * j,
            w * j - x * k + y * v + z * i, w * k + x * j - y * i + z * v};
}
Quaternion conjugate(const Quaternion &value) {
    return {value[0], -value[1], -value[2], -value[3]};
}
Vector3 rotate(const Quaternion &q, const Vector3 &value) {
    const auto result = multiply(multiply(q, {0, value[0], value[1], value[2]}), conjugate(q));
    return {result[1], result[2], result[3]};
}
Quaternion integrate(const Quaternion &q, const Vector3 &angular_velocity, double dt) {
    if (!(dt > 0 && dt < .05) || !std::isfinite(dt) || !finite(q) || !finite(angular_velocity) ||
        std::abs(dot(q, q) - 1) > .001 || norm(angular_velocity) > 100)
        throw std::invalid_argument("invalid orientation sample");
    const double speed = norm(angular_velocity);
    const double factor = speed ? std::sin(speed * dt / 2) / speed : dt / 2;
    auto result = multiply(q, {std::cos(speed * dt / 2), angular_velocity[0] * factor,
                               angular_velocity[1] * factor, angular_velocity[2] * factor});
    const double magnitude = std::sqrt(dot(result, result));
    for (auto &value : result)
        value /= magnitude;
    return result;
}
Vector3 translation(const RigidPose &pose) {
    return {pose[3], pose[7], pose[11]};
}
void validate_pose(const RigidPose &pose) {
    if (!std::all_of(pose.begin(), pose.end(), [](double v) { return std::isfinite(v); }))
        throw std::invalid_argument("invalid rigid pose");
    const std::array<Vector3, 3> axes{
        {{pose[0], pose[4], pose[8]}, {pose[1], pose[5], pose[9]}, {pose[2], pose[6], pose[10]}}};
    for (const auto &axis : axes)
        if (std::abs(dot(axis, axis) - 1) > .001)
            throw std::invalid_argument("pose rotation must be right-handed and orthonormal");
    if (std::abs(dot(axes[0], axes[1])) > .001 || std::abs(dot(axes[0], axes[2])) > .001 ||
        std::abs(dot(axes[1], axes[2])) > .001 || dot(cross(axes[0], axes[1]), axes[2]) < .999)
        throw std::invalid_argument("pose rotation must be right-handed and orthonormal");
}
RigidPose axes_transform(const Vector3 &x, const Vector3 &z, const Vector3 &position) {
    if (!finite(x) || !finite(z) || !finite(position))
        throw std::invalid_argument("invalid rigid frame");
    if (std::abs(dot(x, x) - 1) > 1e-4 || std::abs(dot(z, z) - 1) > 1e-4 ||
        std::abs(dot(x, z)) > 1e-4)
        throw std::invalid_argument("rigid axes must be orthonormal");
    const auto y = cross(z, x);
    return {x[0], y[0],        z[0], position[0], x[1], y[1],
            z[1], position[1], x[2], y[2],        z[2], position[2]};
}
Vector3 transform_point(const RigidPose &pose, const Vector3 &point) {
    validate_pose(pose);
    if (!finite(point))
        throw std::invalid_argument("invalid 3D point");
    Vector3 result{};
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 3; ++j)
            result[i] += pose[i * 4 + j] * point[j];
        result[i] += pose[i * 4 + 3];
    }
    return result;
}
RigidPose inverse_pose(const RigidPose &pose) {
    validate_pose(pose);
    RigidPose result{};
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 3; ++j)
            result[i * 4 + j] = pose[j * 4 + i];
        result[i * 4 + 3] =
            -(result[i * 4] * pose[3] + result[i * 4 + 1] * pose[7] + result[i * 4 + 2] * pose[11]);
    }
    return result;
}
Quaternion quaternion(const RigidPose &pose) {
    validate_pose(pose);
    const double trace = pose[0] + pose[5] + pose[10];
    if (trace > 0) {
        const double s = std::sqrt(trace + 1) * 2;
        return {.25 * s, (pose[9] - pose[6]) / s, (pose[2] - pose[8]) / s, (pose[4] - pose[1]) / s};
    }
    size_t i = 0;
    for (size_t axis = 1; axis < 3; ++axis)
        if (pose[axis * 4 + axis] > pose[i * 4 + i])
            i = axis;
    const size_t j = (i + 1) % 3, k = (i + 2) % 3;
    const double s = std::sqrt(1 + pose[i * 4 + i] - pose[j * 4 + j] - pose[k * 4 + k]) * 2;
    Quaternion q{(pose[k * 4 + j] - pose[j * 4 + k]) / s, 0, 0, 0};
    q[i + 1] = .25 * s;
    q[j + 1] = (pose[j * 4 + i] + pose[i * 4 + j]) / s;
    q[k + 1] = (pose[k * 4 + i] + pose[i * 4 + k]) / s;
    return q;
}
RigidPose rigid_pose(const Quaternion &q, const Vector3 &position) {
    const auto [w, x, y, z] = q;
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     position[0],
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     position[1],
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), position[2]};
}
void PoseHistory::add(double timestamp, const std::optional<RigidPose> &pose) {
    if (!std::isfinite(timestamp) || (!samples.empty() && timestamp <= samples.back().time))
        throw std::invalid_argument("head-pose time must increase");
    if (!pose) {
        samples.clear();
        return;
    }
    validate_pose(*pose);
    samples.push_back({timestamp, *pose, quaternion(*pose)});
    if (samples.size() > 256)
        samples.pop_front();
}
std::optional<RigidPose> PoseHistory::at(double timestamp) const {
    if (!std::isfinite(timestamp))
        throw std::invalid_argument("invalid camera exposure time");
    for (size_t i = 1; i < samples.size(); ++i) {
        const auto &first = samples[i - 1], &second = samples[i];
        if (timestamp < first.time || timestamp > second.time)
            continue;
        const double cosine = dot(first.orientation, second.orientation);
        if (second.time - first.time > .05 || std::abs(cosine) < std::cos(std::acos(-1.) / 8) ||
            distance(translation(first.pose), translation(second.pose)) > .2)
            return {};
        const double a = (timestamp - first.time) / (second.time - first.time);
        Quaternion q{};
        for (size_t k = 0; k < 4; ++k)
            q[k] =
                (1 - a) * first.orientation[k] + a * second.orientation[k] * (cosine >= 0 ? 1 : -1);
        const double magnitude = std::sqrt(dot(q, q));
        for (auto &value : q)
            value /= magnitude;
        return rigid_pose(
            q, ct::add(scale(translation(first.pose), 1 - a), scale(translation(second.pose), a)));
    }
    return {};
}
} // namespace ct
