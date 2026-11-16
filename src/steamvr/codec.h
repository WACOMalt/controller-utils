#pragma once
#include "controller/report.h"
#include "protocol.h"
#include "tracking/native_pose.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ct {
struct InputControls {
    double trigger = 0, grip = 0, stick_x = 0, stick_y = 0, pad_x = 0, pad_y = 0, scroll_x = 0,
           scroll_y = 0;
    bool activate = true;
    bool back = false, bumper = false, stick_touch = false, stick_click = false, pad_touch = false,
         pad_click = false, scroll_touch = false, scroll_click = false;
};
InputControls input_controls(const ControllerReport &report);
std::string controller_profile(const std::string &serial);
wire::Command bridge_event(bool connected, uint64_t hardware_id, const std::string &onboard,
                           const std::string &defaults);
wire::Command bridge_imu(double timestamp, const Vector3 &acceleration,
                         const Vector3 &angular_velocity);
wire::Command bridge_input(double session_start, double timestamp,
                           const std::optional<BodyPoseMessage> &pose,
                           const InputControls &controls, bool enabled);
wire::Status decode_status(const uint8_t *data, size_t size);
NativePoseInput decode_pose(const uint8_t *data, size_t size);
} // namespace ct
