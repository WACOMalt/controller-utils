#include "native_pose.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ct {
namespace {
template <size_t N> void check_vector(const std::array<double, N> &value, double limit) {
    if (std::any_of(value.begin(), value.end(),
                    [limit](double v) { return !std::isfinite(v) || std::abs(v) > limit; }))
        throw std::invalid_argument("invalid native pose vector");
}
} // namespace
NativeBodyPose::NativeBodyPose(const BodyCalibration &calibration, const std::string &serial,
                               const Vector3 &emitter_preview, double smoothing)
    : mount(calibration.body_from_report_rotation), bias(calibration.gyro_bias_report_rad_s),
      emitter(emitter_preview), motion(smoothing), calibration(calibration), serial(serial) {
    if (calibration.schema != 1 || calibration.controller_serial != serial)
        throw std::invalid_argument("body calibration belongs to a different controller");
    if (!std::isfinite(smoothing) || smoothing < 0 || smoothing > .15)
        throw std::invalid_argument("position smoothing must be 0-150 ms");
    RigidPose transform{};
    for (size_t i = 0; i < 3; ++i) {
        check_vector(mount[i], 1);
        for (size_t j = 0; j < 3; ++j)
            transform[i * 4 + j] = mount[i][j];
    }
    validate_pose(transform);
    report_from_body = conjugate(quaternion(transform));
    check_vector(bias, .1);
    check_vector(emitter, .2);
}
Vector3 NativeBodyPose::body_vector(const Vector3 &value) const {
    return {dot(mount[0], value), dot(mount[1], value), dot(mount[2], value)};
}
std::optional<double> NativeBodyPose::sample(const ControllerReport &imu, double received,
                                             double frame_offset,
                                             const std::optional<RigidPose> &head) {
    check_vector(imu.acceleration, 100);
    check_vector(imu.angular_velocity, 100);
    if (!std::isfinite(frame_offset))
        throw std::invalid_argument("invalid native clock offset");
    std::optional<double> mapped;
    try {
        mapped = clock.add(imu.sensor_tick, received, imu.clock_modulus);
    } catch (const std::invalid_argument &error) {
        const std::string message = error.what();
        if (message.rfind("sensor clock reset", 0) != 0 &&
            message.rfind("sensor clock format changed", 0) != 0)
            throw;
        const auto old_scale = clock.scale;
        clock = SensorClock{};
        clock.scale = old_scale;
        clock.offset = received;
        mapped = clock.add(imu.sensor_tick, received, imu.clock_modulus);
        gyro.clear();
        motion.interrupt();
        reset_hands();
        ++imu_recoveries;
    }
    if (!mapped)
        return {};
    const double timestamp = std::min(*mapped, received) + frame_offset;
    if (head && !rotation)
        rotation.emplace(calibration, serial, emitter);
    if (rotation) {
        rotation->sample(imu, received, frame_offset, head);
    }
    const auto omega = body_vector(subtract(imu.angular_velocity, bias));
    const auto up = body_vector(imu.acceleration);
    if (norm(up) < .95 * 9.80665 || norm(up) > 1.05 * 9.80665 || dot(omega, omega) > .08 * .08 ||
        (!alignment_samples.empty() && timestamp - alignment_samples.back().first > .05))
        alignment_samples.clear();
    else if (alignment_samples.empty() || timestamp > alignment_samples.back().first) {
        alignment_samples.emplace_back(timestamp, up);
        if (alignment_samples.size() > 256)
            alignment_samples.pop_front();
    }
    const auto previous =
        gyro.empty() ? std::nullopt : std::optional<std::pair<double, Vector3>>(gyro.back());
    gyro.emplace_back(timestamp, omega);
    if (gyro.size() > 256)
        gyro.pop_front();
    if (rotation) {
        orientation = rotation->orientation;
        if (orientation)
            orientation_time = timestamp;
    } else if (orientation && timestamp > *orientation_time) {
        const double dt = timestamp - *orientation_time;
        if (previous && dt > 0 && dt < .05 && timestamp - previous->first < .05)
            orientation = integrate(*orientation, scale(add(previous->second, omega), .5), dt);
        orientation_time = timestamp;
    }
    if (!motion.time || timestamp > *motion.time) {
        if (motion.time && timestamp - *motion.time > .05) {
            motion.interrupt();
            reset_hands();
        }
        Vector3 force{};
        if (orientation) {
            const auto world = rotate(*orientation, body_vector(imu.acceleration));
            force = {world[0], world[1] - 9.80665, world[2]};
        }
        motion.sample(timestamp, force, norm(omega));
        if (norm(omega) >= .12 || norm(motion.acceleration) >= .35)
            last_motion = timestamp;
    }
    return timestamp;
}
bool NativeBodyPose::observe(const NativePoseInput &pose, double now) {
    const double timestamp = pose.timestamp;
    if (pose.controller_id != 16 || !pose.valid || !std::isfinite(timestamp) || timestamp <= 0 ||
        !std::isfinite(now) || now - timestamp < -.02 || now - timestamp > .15 ||
        (native && timestamp <= native->time))
        return false;
    Quaternion q;
    Vector3 point;
    try {
        if (!pose.orientation || !pose.position)
            throw std::invalid_argument("missing native pose vector");
        q = *pose.orientation;
        point = *pose.position;
        check_vector(q, 1.01);
        const double length = std::sqrt(dot(q, q));
        if (std::abs(length - 1) > .01)
            throw std::invalid_argument("invalid native pose quaternion");
        for (auto &v : q)
            v /= length;
        check_vector(point, 20);
    } catch (const std::invalid_argument &) {
        ++rejected_poses;
        return false;
    }
    auto body_q = multiply(multiply(Quaternion{0, 1, 0, 0}, q), report_from_body);
    const auto native_q = body_q;
    if (rotation) {
        const auto measured = rotation->history.at(timestamp);
        if (measured)
            body_q = quaternion(*measured);
        else if (rotation->orientation && !gyro.empty() &&
                 timestamp - *rotation->orientation_time >= 0 &&
                 timestamp - *rotation->orientation_time <= .02) {
            const double dt = timestamp - *rotation->orientation_time;
            body_q = dt ? integrate(*rotation->orientation, gyro.back().second, dt)
                        : *rotation->orientation;
        } else
            return false;
    }
    const Vector3 raw_point{point[0], -point[1], -point[2]};
    const auto body_point = subtract(raw_point, rotate(body_q, emitter));
    if (!motion.time)
        motion.sample(timestamp, {}, 0);
    const auto fix = motion.observe(timestamp, body_point, now);
    if (fix == InertialPosition::Fix::Rejected) {
        ++rejected_poses;
        return false;
    }
    if (palm_aligned)
        measured_body.add(timestamp, rigid_pose(body_q, body_point));
    hands.native_observed(timestamp, now, body_point, std::abs(timestamp - last_motion) <= .24,
                          fix == InertialPosition::Fix::PalmRelocated);
    native = NativeObservation{timestamp, body_point, body_q, native_q, raw_point};
    if (!rotation) {
        double current = timestamp;
        auto replayed = body_q;
        std::optional<Vector3> previous;
        for (const auto &[sample_time, omega] : gyro) {
            if (sample_time <= timestamp) {
                previous = omega;
                continue;
            }
            const double dt = sample_time - current;
            if (!(dt > 0 && dt < .05)) {
                previous = omega;
                current = sample_time;
                continue;
            }
            const auto velocity = previous ? scale(add(*previous, omega), .5) : omega;
            replayed = integrate(replayed, velocity, dt);
            previous = omega;
            current = sample_time;
        }
        orientation = replayed;
        orientation_time = current;
    }
    return true;
}
void NativeBodyPose::observe_hands(const std::array<double, 2> &stamps, const PalmPoints &palms,
                                   double now) {
    const double stamp = std::max(stamps[0], stamps[1]);
    if (!palm_aligned || !native || !rotation || !orientation_time || !std::isfinite(stamp) ||
        stamp > now || now - stamp > .18 || std::abs(*orientation_time - stamp) > .20)
        return;
    auto q = rotation->history.at(stamp);
    if (!q && stamp >= *orientation_time && stamp - *orientation_time <= .02 && orientation)
        q = rigid_pose(*orientation, {});
    const auto base = motion.value(now);
    if (!q || !base)
        return;
    if (motion.relocation && motion.relocation->palms) {
        for (size_t i = 0; i < palms.size(); ++i) {
            std::optional<Vector3> center;
            if (palms[i] && std::isfinite(stamps[i]) && now - stamps[i] <= .18) {
                const auto pose = stamps[i] == stamp ? q : rotation->history.at(stamps[i]);
                if (pose)
                    center = hands.grip_center(i, stamps[i], *palms[i], quaternion(*pose));
            }
            motion.observe_grip(i, palms[i] ? stamps[i] : stamp, center);
        }
    }
    const auto past = measured_body.at(stamp);
    std::optional<Vector3> measurement;
    if (past)
        measurement = translation(*past);
    else if (std::abs(stamp - native->time) <= .045)
        measurement = native->position;
    hands.observe(stamp, now, palms, quaternion(*q), measurement, hands.value(now, *base),
                  std::abs(stamp - last_motion) <= .24);
}
void NativeBodyPose::reset_hands() {
    hands = {};
    measured_body = {};
    if (motion.relocation && motion.relocation->palms)
        motion.relocation.reset();
}
void NativeBodyPose::recenter() {
    reset_hands();
    rotation.emplace(calibration, serial, emitter);
    orientation.reset();
    orientation_time.reset();
    palm_aligned = false;
}
void NativeBodyPose::alignment_ready(double now, const RigidPose &head) const {
    validate_pose(head);
    if (!strict_native(now))
        throw std::invalid_argument("Keep the gamepad's top edge visible to Frame.");
    size_t count = 0;
    for (const auto &[time, value] : alignment_samples) {
        (void)value;
        if (time >= now - .4 && time <= now + .02)
            ++count;
    }
    if (!rotation || !rotation->orientation || count < 20 || alignment_samples.empty() ||
        now - alignment_samples.back().first > .05 ||
        alignment_samples.back().first - alignment_samples[alignment_samples.size() - count].first <
            .2)
        throw std::invalid_argument("Hold the gamepad still for a moment.");
    const double separation = distance(native->emitter, translation(head));
    if (separation < .1 || separation > 1.4)
        throw std::invalid_argument("Hold the gamepad in front of Frame while tracking settles.");
}
void NativeBodyPose::align_head(double now, const RigidPose &head) {
    alignment_ready(now, head);
    const auto backward = rotate(*rotation->orientation, {0, 0, 1});
    if (std::min(std::hypot(head[2], head[10]), std::hypot(backward[0], backward[2])) < .25)
        throw std::invalid_argument("Look ahead and point the gamepad's top edge the same way.");
    apply_heading(std::atan2(head[2], head[10]) - std::atan2(backward[0], backward[2]), now);
}
bool NativeBodyPose::resume(const TrackingCheckpoint &saved, const TrackingSpace &space, double now,
                            const RigidPose &head) {
    if (saved.serial != serial || saved.space.universe != space.universe ||
        now <= saved.pose.time || now - saved.pose.time > 300)
        return false;
    alignment_ready(now, head);
    const auto standing_to_raw = inverse_pose(space.raw_to_standing);
    const auto previous_emitter = transform_point(
        standing_to_raw, transform_point(saved.space.raw_to_standing, saved.pose.emitter));
    const auto previous_to_raw =
        multiply(quaternion(standing_to_raw), quaternion(saved.space.raw_to_standing));
    // A single LED cannot confirm yaw. Check the native orientation before restoring heading.
    if (distance(previous_emitter, native->emitter) > .06 ||
        std::abs(dot(multiply(previous_to_raw, saved.pose.native_orientation),
                     native->native_orientation)) < .985)
        return false;
    const auto previous = multiply(previous_to_raw, saved.pose.orientation);
    const auto previous_back = rotate(previous, {0, 0, 1});
    const auto current_back = rotate(*rotation->orientation, {0, 0, 1});
    const auto previous_up = rotate(conjugate(previous), {0, 1, 0});
    const auto current_up = rotate(conjugate(*rotation->orientation), {0, 1, 0});
    if (dot(previous_up, current_up) < .99 ||
        std::min(std::hypot(previous_back[0], previous_back[2]),
                 std::hypot(current_back[0], current_back[2])) < .25)
        return false;
    apply_heading(std::atan2(previous_back[0], previous_back[2]) -
                      std::atan2(current_back[0], current_back[2]),
                  now);
    const auto previous_body = subtract(previous_emitter, rotate(previous, emitter));
    motion.blend = InertialPosition::Blend{now, subtract(previous_body, native->position), .15};
    return true;
}
void NativeBodyPose::apply_heading(double yaw, double now) {
    if (!rotation || !rotation->orientation || !native)
        throw std::logic_error("gamepad heading is not ready");
    const Quaternion correction{std::cos(yaw / 2), 0, std::sin(yaw / 2), 0};
    const auto corrected = multiply(correction, *rotation->orientation);
    const auto measured = multiply(correction, native->orientation);
    const auto point = subtract(native->emitter, rotate(measured, emitter));
    const auto previous = motion.value(now);
    orientation = rotation->orientation = corrected;
    rotation->history = PoseHistory{};
    motion = InertialPosition(motion.smoothing);
    motion.sample(now, {}, 0);
    motion.observe(native->time, point, now);
    if (previous && distance(*previous, point) < .25 && motion.smoothing)
        motion.blend = InertialPosition::Blend{now, subtract(*previous, point), motion.smoothing};
    native->position = point;
    native->orientation = measured;
    reset_hands();
    palm_aligned = true;
}
std::optional<NativeObservation> NativeBodyPose::strict_native(double now) const {
    if (!native || !std::isfinite(now) || now - native->time < -.02 || now - native->time > .15)
        return {};
    return native;
}
std::optional<BodyPoseMessage> NativeBodyPose::input_pose(double now) const {
    const auto hand_time = hands.recent(now);
    if (!strict_native(now) && !hand_time)
        return {};
    auto pose = message(now);
    if (pose && hand_time)
        pose->time = std::max(pose->time, *hand_time);
    return pose;
}
std::optional<BodyPoseMessage> NativeBodyPose::message(double now) const {
    if (!native || !orientation_time || !std::isfinite(now) || now - *orientation_time < -.02 ||
        now - *orientation_time > .15)
        return {};
    const auto position = motion.value(now);
    if (!position)
        return {};
    return BodyPoseMessage{native->time, now, hands.value(now, *position), *orientation_time,
                           *orientation};
}
} // namespace ct
