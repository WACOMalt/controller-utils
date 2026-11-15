#pragma once
#include "orientation.h"
#include "position.h"
#include <deque>
#include <optional>

namespace ct {
struct NativePoseInput {
    uint32_t controller_id = 0;
    double timestamp = -1;
    bool valid = false;
    std::optional<Quaternion> orientation;
    std::optional<Vector3> position;
    Vector3 velocity{}, angular_velocity{}, acceleration{};
};
struct NativeObservation {
    double time;
    Vector3 position;
    Quaternion orientation, native_orientation;
    Vector3 emitter;
};
struct BodyPoseMessage {
    double time, position_time;
    Vector3 position;
    double orientation_time;
    Quaternion orientation;
};

class NativeBodyPose {
  public:
    std::array<Vector3, 3> mount;
    Quaternion report_from_body;
    Vector3 bias, emitter;
    SensorClock clock;
    InertialPosition motion;
    std::deque<std::pair<double, Vector3>> gyro;
    std::optional<Quaternion> orientation;
    std::optional<double> orientation_time;
    std::optional<NativeObservation> native;
    unsigned imu_recoveries = 0, rejected_poses = 0;
    std::optional<OrientationFilter> rotation;
    BodyCalibration calibration;
    std::string serial;
    std::deque<std::pair<double, Vector3>> alignment_samples;
    bool palm_aligned = false;

    NativeBodyPose(const BodyCalibration &calibration, const std::string &serial,
                   const Vector3 &emitter_preview, double smoothing = .04);
    Vector3 body_vector(const Vector3 &value) const;
    std::optional<double> sample(const ControllerReport &imu, double received, double frame_offset,
                                 const std::optional<RigidPose> &head = std::nullopt);
    bool observe(const NativePoseInput &pose, double now);
    void recenter();
    void alignment_ready(double now, const RigidPose &head) const;
    void align_head(double now, const RigidPose &head);
    void apply_heading(double yaw, double now);
    std::optional<NativeObservation> strict_native(double now) const;
    std::optional<BodyPoseMessage> input_pose(double now) const;
    std::optional<BodyPoseMessage> message(double now) const;
};
} // namespace ct
