#pragma once
#include "controller/sensor_clock.h"
#include "math.h"
#include <array>
#include <deque>
#include <optional>
#include <string>

namespace ct {
struct BodyCalibration {
    unsigned schema = 1;
    std::string controller_serial;
    std::array<Vector3, 3> body_from_report_rotation{};
    Vector3 gyro_bias_report_rad_s{};
};

Vector3 unit(const Vector3 &value);
Quaternion recenter(const Vector3 &gravity, const RigidPose &head);

class OrientationFilter {
  public:
    std::array<Vector3, 3> mount;
    Vector3 bias, emitter;
    SensorClock clock;
    PoseHistory history;
    std::deque<Vector3> still;
    std::optional<std::pair<double, Vector3>> previous;
    std::optional<Quaternion> orientation;
    std::optional<double> orientation_time, recenter_time;
    Vector3 acceleration_bias{};
    unsigned imu_recoveries = 0;
    OrientationFilter(const BodyCalibration &calibration, const std::string &serial,
                      const Vector3 &emitter);
    Vector3 body_vector(const Vector3 &value) const;
    void sample(const ControllerReport &imu, double received, double frame_offset,
                const std::optional<RigidPose> &head, double gravity_gain = 2.);
};
} // namespace ct
