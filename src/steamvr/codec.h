#pragma once
#include "controller/report.h"
#include "protocol.h"
#include "tracking/body_pose.h"
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
wire::AppInput bridge_input(double session_start, double timestamp,
                            const std::optional<BodyPoseMessage> &pose,
                            const InputControls &controls, bool enabled);
} // namespace ct
