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
} // namespace ct
