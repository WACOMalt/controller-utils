#include "report.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ct {
namespace {
uint16_t u16(const uint8_t *data) {
    return uint16_t(data[0]) | uint16_t(data[1]) << 8;
}
int16_t i16(const uint8_t *data) {
    const auto value = u16(data);
    return value < 0x8000 ? int16_t(value) : int16_t(int32_t(value) - 0x10000);
}
uint32_t u32(const uint8_t *data) {
    return uint32_t(u16(data)) | uint32_t(u16(data + 2)) << 16;
}
} // namespace

ControllerReport decode_report(const uint8_t *data, size_t size) {
    if (!size || (data[0] != 0x42 && data[0] != 0x45 && data[0] != 0x47) ||
        (data[0] == 0x47 ? size != 46 : size != 46 && size != 54))
        throw std::invalid_argument("unexpected Steam Controller 2026 state report");
    ControllerReport result;
    result.sequence = data[1];
    result.buttons = u32(data + 2);
    for (size_t i = 0; i < 2; ++i) {
        result.triggers[i] = std::clamp(i16(data + 6 + i * 2) / 32767., 0., 1.);
        result.left_stick[i] = i16(data + 10 + i * 2) / 32768.;
        result.right_stick[i] = i16(data + 14 + i * 2) / 32768.;
    }
    const auto pads = data + (data[0] == 0x47 ? 20 : 18);
    result.left_pad = {i16(pads) / 32768., i16(pads + 2) / 32768., u16(pads + 4) / 32768.};
    result.right_pad = {i16(pads + 6) / 32768., i16(pads + 8) / 32768., u16(pads + 10) / 32768.};
    result.sensor_tick = data[0] == 0x47 ? uint64_t(u16(data + 32)) * 32 : u32(data + 30);
    result.clock_modulus = uint64_t(1) << (data[0] == 0x47 ? 21 : 32);
    for (size_t i = 0; i < 3; ++i) {
        result.acceleration[i] = i16(data + 34 + i * 2) * 9.80665 / 16384.;
        result.angular_velocity[i] = (i16(data + 40 + i * 2) / 16.384) * (std::acos(-1.) / 180.);
    }
    return result;
}

std::array<uint8_t, 64> setting_report(uint32_t setting, uint32_t value) {
    if ((setting != 0x24 && setting != 0x30) || value > 65535)
        throw std::invalid_argument("unsupported transient setting");
    if (setting == 0x24 && value > 100)
        throw std::invalid_argument("IR brightness must be 0-100");
    if (setting == 0x30 && value != 0 && value != 0x18)
        throw std::invalid_argument("unsupported IMU mode");
    std::array<uint8_t, 64> result{};
    result[0] = 1;
    result[1] = 0x87;
    result[2] = 3;
    result[3] = uint8_t(setting);
    result[4] = uint8_t(value);
    result[5] = uint8_t(value >> 8);
    return result;
}

std::string parse_serial(const uint8_t *data, size_t size) {
    if (size != 64 || data[0] != 1 || data[1] != 0xae || data[2] != 0x14 || data[3] != 1)
        throw std::invalid_argument("unexpected serial response");
    std::string result;
    for (size_t i = 4; i < 23 && data[i]; ++i) {
        if (!((data[i] >= '0' && data[i] <= '9') || (data[i] >= 'A' && data[i] <= 'Z')))
            throw std::invalid_argument("invalid controller serial");
        result += char(data[i]);
    }
    if (result.size() < 8)
        throw std::invalid_argument("invalid controller serial");
    return result;
}
} // namespace ct
