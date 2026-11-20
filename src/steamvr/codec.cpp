#include "codec.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ct {
InputControls input_controls(const ControllerReport &state) {
    const auto &triggers = state.triggers, &stick = state.right_stick;
    const auto &pad = state.right_pad, &scroll = state.left_pad;
    if (std::any_of(triggers.begin(), triggers.end(),
                    [](double v) { return !std::isfinite(v) || v < 0 || v > 1; }) ||
        std::any_of(stick.begin(), stick.end(),
                    [](double v) { return !std::isfinite(v) || std::abs(v) > 1; }) ||
        !finite(pad) || !finite(scroll) || std::abs(pad[0]) > 1 || std::abs(pad[1]) > 1 ||
        std::abs(scroll[0]) > 1 || std::abs(scroll[1]) > 1 || pad[2] < 0 || pad[2] >= 2 ||
        scroll[2] < 0 || scroll[2] >= 2)
        throw std::invalid_argument("invalid native controller input");
    const auto buttons = state.buttons;
    InputControls result;
    result.trigger = triggers[1];
    result.grip = triggers[0];
    result.stick_x = stick[0];
    result.stick_y = stick[1];
    result.pad_x = pad[0];
    result.pad_y = pad[1];
    result.pad_touch = buttons & 0x00200000;
    result.pad_click = buttons & 0x00400000;
    result.scroll_x = scroll[0];
    result.scroll_y = scroll[1];
    result.scroll_touch = buttons & 0x02000000;
    result.scroll_click = buttons & 0x04000000;
    result.back = buttons & 2;
    result.bumper = buttons & 0x200;
    result.stick_click = buttons & 0x20;
    result.stick_touch = (buttons & 0x20) || std::hypot(stick[0], stick[1]) > .08;
    return result;
}
wire::AppInput bridge_input(double session_start, double timestamp,
                            const std::optional<BodyPoseMessage> &pose,
                            const InputControls &controls, bool enabled) {
    const double pose_time = pose ? pose->time : 0;
    const Quaternion orientation = pose ? pose->orientation : Quaternion{1, 0, 0, 0};
    const Vector3 position = pose ? pose->position : Vector3{};
    const std::array<double, 8> analog{controls.trigger,  controls.grip,    controls.stick_x,
                                       controls.stick_y,  controls.pad_x,   controls.pad_y,
                                       controls.scroll_x, controls.scroll_y};
    if (!std::isfinite(session_start) || !std::isfinite(timestamp) || !std::isfinite(pose_time) ||
        !finite(orientation) || !finite(position) || session_start <= 0 ||
        timestamp < session_start || std::abs(dot(orientation, orientation) - 1) > .001 ||
        std::any_of(position.begin(), position.end(), [](double v) { return std::abs(v) > 20; }) ||
        std::any_of(analog.begin(), analog.end(), [](double v) { return !std::isfinite(v); }) ||
        analog[0] < 0 || analog[0] > 1 || analog[1] < 0 || analog[1] > 1 ||
        std::any_of(analog.begin() + 2, analog.end(), [](double v) { return std::abs(v) > 1; }) ||
        (pose && (timestamp - pose_time < -.02 || timestamp - pose_time > .15)))
        throw std::invalid_argument("invalid native input state");
    wire::AppInput input{};
    input.id = wire::ControllerId;
    input.flags = unsigned(enabled) | (pose ? 2u : 0u) | (enabled && !controls.activate ? 4u : 0u);
    input.session_start = session_start;
    input.time = timestamp;
    input.pose_time = pose_time;
    std::copy(orientation.begin(), orientation.end(), input.orientation);
    std::copy(position.begin(), position.end(), input.position);
    input.trigger = float(controls.trigger);
    input.grip = float(controls.grip);
    input.stick_x = float(controls.stick_x);
    input.stick_y = float(controls.stick_y);
    input.pad_x = float(controls.pad_x);
    input.pad_y = float(controls.pad_y);
    input.scroll_x = float(controls.scroll_x);
    input.scroll_y = float(controls.scroll_y);
    const std::array<bool, 8> buttons{
        controls.back,      controls.bumper,    controls.stick_touch,  controls.stick_click,
        controls.pad_touch, controls.pad_click, controls.scroll_touch, controls.scroll_click};
    for (size_t i = 0; i < buttons.size(); ++i)
        if (buttons[i])
            input.buttons |= 1u << i;
    return input;
}
} // namespace ct
