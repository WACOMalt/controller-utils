#include "orientation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ct {
Vector3 unit(const Vector3 &value) {
    const double length = norm(value);
    if (length < 1e-6)
        throw std::invalid_argument("cannot normalize a zero direction");
    return scale(value, 1 / length);
}
Quaternion recenter(const Vector3 &gravity, const RigidPose &head) {
    validate_pose(head);
    const auto up = unit(gravity);
    auto backward = subtract(Vector3{0, 0, 1}, scale(up, up[2]));
    if (dot(backward, backward) < .25 * .25)
        throw std::invalid_argument("hold the gamepad approximately level to recenter");
    backward = unit(backward);
    const auto right = cross(up, backward);
    const auto world_backward = unit({head[2], 0, head[10]});
    const auto world_right = cross({0, 1, 0}, world_backward);
    const std::array<Vector3, 3> body_axes{right, up, backward},
        world_axes{world_right, Vector3{0, 1, 0}, world_backward};
    RigidPose rotation{};
    for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 3; ++c)
            for (size_t k = 0; k < 3; ++k)
                rotation[r * 4 + c] += world_axes[k][r] * body_axes[k][c];
    return quaternion(rotation);
}
OrientationFilter::OrientationFilter(const BodyCalibration &calibration, const std::string &serial,
                                     const Vector3 &emitter)
    : mount(calibration.body_from_report_rotation), bias(calibration.gyro_bias_report_rad_s),
      emitter(emitter) {
    if (calibration.schema != 1 || calibration.controller_serial != serial)
        throw std::invalid_argument("body calibration belongs to a different controller");
    RigidPose rotation{};
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            rotation[i * 4 + j] = mount[i][j];
    validate_pose(rotation);
    if (!finite(bias) ||
        std::any_of(bias.begin(), bias.end(), [](double v) { return std::abs(v) >= .1; }))
        throw std::invalid_argument("invalid calibrated gyro bias");
    if (!finite(emitter) ||
        std::any_of(emitter.begin(), emitter.end(), [](double v) { return std::abs(v) >= .2; }))
        throw std::invalid_argument(
            "supply the calibrated emitter offset or an explicit preview offset");
}
Vector3 OrientationFilter::body_vector(const Vector3 &value) const {
    return {dot(mount[0], value), dot(mount[1], value), dot(mount[2], value)};
}
void OrientationFilter::sample(const ControllerReport &imu, double received, double frame_offset,
                               const std::optional<RigidPose> &head, double gravity_gain) {
    const auto &acceleration = imu.acceleration, &angular = imu.angular_velocity;
    if (!finite(acceleration) || !finite(angular) ||
        std::any_of(acceleration.begin(), acceleration.end(),
                    [](double v) { return std::abs(v) >= 100; }) ||
        std::any_of(angular.begin(), angular.end(), [](double v) { return std::abs(v) >= 100; }))
        throw std::invalid_argument("invalid body IMU sample");
    const auto modulus = imu.clock_modulus;
    if ((modulus != uint64_t(1) << 21 && modulus != uint64_t(1) << 32) ||
        imu.sensor_tick >= modulus || !std::isfinite(received))
        throw std::invalid_argument("invalid clock sample");
    if (clock.last_tick && (imu.sensor_tick != *clock.last_tick || clock.modulus != modulus)) {
        const auto delta = (imu.sensor_tick + modulus - *clock.last_tick) % modulus;
        if (clock.modulus != modulus || received - clock.points.back().second > 1 ||
            delta * clock.scale.value_or(1) * 1e-6 > .05) {
            const auto old_scale = clock.scale;
            clock = SensorClock{};
            clock.scale = old_scale;
            clock.offset = received;
            previous.reset();
            history = PoseHistory{};
            still.clear();
            ++imu_recoveries;
        }
    }
    const auto timestamp = clock.add(imu.sensor_tick, received, modulus);
    const double elapsed = clock.elapsed * 1e-6;
    if (previous && elapsed == previous->first)
        return;
    const auto up = body_vector(acceleration), omega = body_vector(subtract(angular, bias));
    const auto before = previous;
    previous = std::pair<double, Vector3>{elapsed, omega};
    const double gravity = norm(up);
    if (!orientation) {
        if (gravity > .9 * 9.80665 && gravity < 1.1 * 9.80665 && dot(omega, omega) < .1 * .1) {
            still.push_back(up);
            if (still.size() > 25)
                still.pop_front();
        } else
            still.clear();
        if (!timestamp || !head || still.size() < 25)
            return;
        Vector3 mean{};
        for (const auto &v : still)
            mean = add(mean, v);
        mean = scale(mean, 1 / double(still.size()));
        const auto direction = unit(mean);
        if (1 - direction[2] * direction[2] < .25 * .25) {
            validate_pose(*head);
            return;
        }
        orientation = recenter(mean, *head);
        recenter_time = *timestamp + frame_offset;
        acceleration_bias = subtract(mean, scale(direction, 9.80665));
    } else if (before) {
        const double dt = (elapsed - before->first) * *clock.scale;
        auto corrected = scale(add(omega, before->second), .5);
        const auto predicted = rotate(conjugate(*orientation), {0, 1, 0});
        if (gravity > .9 * 9.80665 && gravity < 1.1 * 9.80665) {
            const auto measured = unit(up);
            if (dot(measured, predicted) > std::cos(20 * std::acos(-1.) / 180))
                corrected = add(corrected, scale(cross(measured, predicted), gravity_gain));
        }
        orientation = integrate(*orientation, corrected, dt);
    }
    if (!timestamp)
        return;
    orientation_time = *timestamp + frame_offset;
    history.add(*orientation_time, rigid_pose(*orientation, {}));
}
} // namespace ct
