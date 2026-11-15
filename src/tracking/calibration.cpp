#include "calibration.h"
#include "native_pose.h"
#include "platform/linux/files.h"
#include "platform/linux/records.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <system_error>

namespace ct {
namespace {
std::string name(const std::string &serial) {
    if (serial.empty() || serial.size() > 128 ||
        std::any_of(serial.begin(), serial.end(),
                    [](unsigned char c) { return c < 32 || c > 126; }))
        throw std::invalid_argument("Invalid controller calibration identity");
    return sha256_bytes(serial.data(), serial.size()) + ".json";
}
ProtectedDirectory directory(const std::filesystem::path &data) {
    ProtectedDirectory parent(data, getuid());
    if (mkdirat(parent.descriptor(), "calibrations", 0700) && errno != EEXIST)
        throw std::system_error(errno, std::generic_category(), "create controller calibrations");
    parent.verify();
    return ProtectedDirectory(data / "calibrations", getuid());
}
void validate(const TrackingCalibration &value) {
    name(value.body.controller_serial);
    NativeBodyPose checked(value.body, value.body.controller_serial, value.emitter);
}
} // namespace
TrackingCalibration TrackingCalibration::from_json(json_object *value) {
    TrackingCalibration result;
    if (const auto field = json::find(value, "headset_serial"))
        result.headset_serial = json::string(field);
    result.emitter = json::numbers<3>(json::field(value, "emitter_preview"));
    auto body = json::field(value, "body");
    const auto schema = json::integer(json::field(body, "schema"));
    if (schema != 1)
        throw std::invalid_argument("unsupported body calibration");
    result.body.schema = unsigned(schema);
    result.body.controller_serial = json::string(json::field(body, "controller_serial"));
    const auto rotation = json::field(body, "body_from_report_rotation");
    if (json::array_size(rotation) != 3)
        throw std::invalid_argument("invalid IMU body rotation");
    for (size_t i = 0; i < 3; ++i)
        result.body.body_from_report_rotation[i] = json::numbers<3>(json::item(rotation, i));
    result.body.gyro_bias_report_rad_s =
        json::numbers<3>(json::field(body, "gyro_bias_report_rad_s"));
    if (const auto field = json::find(value, "gyro_calibrated"))
        result.gyro_calibrated = json::boolean(field);
    validate(result);
    return result;
}
json::Value TrackingCalibration::to_json() const {
    validate(*this);
    json::Value value(json_object_new_object()), body_value(json_object_new_object()),
        rotation(json_object_new_array());
    for (const auto &row : body.body_from_report_rotation)
        json_object_array_add(rotation.get(), json::value(row));
    json_object_object_add(body_value.get(), "schema", json_object_new_int(body.schema));
    json_object_object_add(body_value.get(), "controller_serial",
                           json_object_new_string(body.controller_serial.c_str()));
    json_object_object_add(body_value.get(), "body_from_report_rotation", rotation.release());
    json_object_object_add(body_value.get(), "gyro_bias_report_rad_s",
                           json::value(body.gyro_bias_report_rad_s));
    json_object_object_add(value.get(), "schema", json_object_new_int(1));
    json_object_object_add(value.get(), "body", body_value.release());
    json_object_object_add(value.get(), "emitter_preview", json::value(emitter));
    json_object_object_add(value.get(), "gyro_calibrated",
                           json_object_new_boolean(gyro_calibrated));
    return value;
}
void save_controller_calibration(const std::filesystem::path &data,
                                 const TrackingCalibration &calibration) {
    const auto contents = calibration.to_json();
    const auto store = directory(data);
    write_record(store, name(calibration.body.controller_serial), json::encode(contents.get()));
}
TrackingCalibration controller_calibration(const std::filesystem::path &data,
                                           const std::string &headset, const std::string &serial) {
    const auto store = directory(data);
    const auto filename = name(serial);
    if (const auto bytes = read_record(store, filename, 131072)) {
        auto result = TrackingCalibration::from_json(json::parse(*bytes).get());
        if (result.body.controller_serial != serial)
            throw std::runtime_error("Controller calibration identity changed");
        result.headset_serial = headset;
        return result;
    }
    const double angle = 100 * std::acos(-1.) / 180;
    TrackingCalibration result{headset,
                               {1,
                                serial,
                                {{{1, 0, 0},
                                  {0, std::cos(angle), std::sin(angle)},
                                  {0, -std::sin(angle), std::cos(angle)}}},
                                {}},
                               {0, -.0004572075, .0087},
                               false};
    save_controller_calibration(data, result);
    return result;
}
std::optional<Vector3> StationaryGyroBias::sample(const ControllerReport &report, double time) {
    if (!std::isfinite(time) || !finite(report.acceleration) || !finite(report.angular_velocity))
        throw std::invalid_argument("Invalid stationary calibration sample");
    if (last_ && time <= *last_)
        return {};
    if ((last_ && time - *last_ > .05) ||
        std::abs(norm(report.acceleration) - 9.80665) > .05 * 9.80665 ||
        norm(report.angular_velocity) > .02) {
        *this = StationaryGyroBias{};
        return {};
    }
    if (!first_)
        first_ = time;
    last_ = time;
    ++count_;
    for (size_t i = 0; i < 3; ++i) {
        const double a = report.acceleration[i], g = report.angular_velocity[i];
        angular_[i] += g;
        angular_squared_[i] += g * g;
        acceleration_[i] += a;
        acceleration_squared_[i] += a * a;
    }
    if (time - *first_ < .75 || count_ < 32)
        return {};
    const auto mean = scale(angular_, 1. / count_);
    double av = 0, gv = 0;
    for (size_t i = 0; i < 3; ++i) {
        gv += std::max(0., angular_squared_[i] / count_ - mean[i] * mean[i]);
        av += std::max(0.,
                       acceleration_squared_[i] / count_ - std::pow(acceleration_[i] / count_, 2));
    }
    if (gv > .002 * .002 || av > .05 * .05 || norm(mean) > .01) {
        *this = StationaryGyroBias{};
        return {};
    }
    return mean;
}
} // namespace ct
