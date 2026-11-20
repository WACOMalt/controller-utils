#pragma once
#include "orientation.h"
#include "platform/json.h"
#include <filesystem>

namespace ct {
struct TrackingCalibration {
    std::string headset_serial;
    BodyCalibration body;
    Vector3 emitter;
    bool gyro_calibrated = true;
    static TrackingCalibration from_json(json_object *value);
    json::Value to_json() const;
};
TrackingCalibration controller_calibration(const std::filesystem::path &data,
                                           const std::string &headset, const std::string &serial);
void save_controller_calibration(const std::filesystem::path &data,
                                 const TrackingCalibration &calibration);
class StationaryGyroBias {
    std::optional<double> first_, last_;
    unsigned count_ = 0;
    Vector3 angular_{}, angular_squared_{}, acceleration_{}, acceleration_squared_{};

  public:
    std::optional<Vector3> sample(const ControllerReport &report, double time);
};
} // namespace ct
