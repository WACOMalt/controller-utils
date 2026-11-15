#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ct {
using Vector3 = std::array<double, 3>;
using Quaternion = std::array<double, 4>;

struct ControllerReport {
    uint8_t sequence = 0;
    uint32_t buttons = 0;
    std::array<double, 2> triggers{}, left_stick{}, right_stick{};
    Vector3 left_pad{}, right_pad{}, acceleration{}, angular_velocity{};
    uint64_t sensor_tick = 0, clock_modulus = uint64_t(1) << 32;
};

struct ControllerInput {
    ControllerReport report;
    double received = 0;
};

ControllerReport decode_report(const uint8_t *data, size_t size);
inline ControllerReport decode_report(const std::vector<uint8_t> &data) {
    return decode_report(data.data(), data.size());
}
std::array<uint8_t, 64> setting_report(uint32_t setting, uint32_t value);
std::string parse_serial(const uint8_t *data, size_t size);
} // namespace ct
