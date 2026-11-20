#include "app_replay.h"
#include "app/settings.h"
#include "controller/report.h"
#include "controller/sensor_clock.h"
#include "interaction/gestures.h"
#include "platform/json.h"
#include "steamvr/codec.h"
#include "steamvr/steam_ui_transport.h"
#include "tracking/acquisition.h"
#include "tracking/math.h"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>

namespace j = ct::json;
namespace {
std::vector<uint8_t> bytes(json_object *value) {
    const auto text = j::string(value);
    if (text.size() % 2)
        throw std::invalid_argument("invalid hexadecimal bytes");
    std::vector<uint8_t> result;
    auto digit = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        throw std::invalid_argument("invalid hexadecimal bytes");
    };
    for (size_t i = 0; i < text.size(); i += 2)
        result.push_back(uint8_t(digit(text[i]) * 16 + digit(text[i + 1])));
    return result;
}
std::string hex(const uint8_t *data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < size; ++i) {
        result += digits[data[i] >> 4];
        result += digits[data[i] & 15];
    }
    return result;
}
uint32_t buttons(json_object *value) {
    const auto number = j::integer(value);
    if (number < 0 || uint64_t(number) > UINT32_MAX)
        throw std::invalid_argument("invalid button mask");
    return uint32_t(number);
}
void set(json_object *object, const char *key, json_object *value) {
    json_object_object_add(object, key, value);
}
void set(json_object *object, const char *key, bool value) {
    set(object, key, json_object_new_boolean(value));
}
void set(json_object *object, const char *key, const std::string &value) {
    set(object, key, json_object_new_string(value.c_str()));
}
void set(json_object *object, const char *key, std::optional<double> value) {
    set(object, key, j::value(value));
}
json_object *encode(const ct::ControllerReport &report) {
    auto out = json_object_new_object();
    set(out, "sequence", json_object_new_int(report.sequence));
    set(out, "buttons", json_object_new_int64(report.buttons));
    set(out, "triggers", j::value(report.triggers));
    set(out, "left_stick", j::value(report.left_stick));
    set(out, "right_stick", j::value(report.right_stick));
    set(out, "left_pad", j::value(report.left_pad));
    set(out, "right_pad", j::value(report.right_pad));
    set(out, "sensor_tick", json_object_new_int64(report.sensor_tick));
    set(out, "clock_modulus", json_object_new_int64(report.clock_modulus));
    set(out, "acceleration", j::value(report.acceleration));
    set(out, "angular_velocity", j::value(report.angular_velocity));
    return out;
}
size_t descriptors() {
    const std::filesystem::directory_iterator end;
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"), end);
}

j::Value replay(json_object *request) {
    if (j::string(j::field(request, "component")) == "application")
        return app_replay::replay(request);
    if (j::string(j::field(request, "component")) == "steam_ui_transport") {
        const auto before = descriptors();
        j::Value result(json_object_new_object()), replies(json_object_new_array());
        const auto connections = j::integer(j::field(request, "connections"));
        const auto count = j::integer(j::field(request, "requests"));
        if (connections < 1 || connections > 100 || count < 1 || count > 100)
            throw std::invalid_argument("Invalid Steam UI transport test size");
        for (int i = 0; i < connections; ++i) {
            auto connection = ct::connect_steam_ui();
            ct::SteamUiRequest update{"TEST1234", "OWNER"};
            update.enabled = true;
            for (int n = 0; n < count; ++n)
                json_object_array_add(replies.get(), connection->renew(update).release());
        }
        set(result.get(), "replies", replies.release());
        set(result.get(), "fds_before", json_object_new_uint64(before));
        set(result.get(), "fds_after", json_object_new_uint64(descriptors()));
        return result;
    }
    const auto component = j::string(j::field(request, "component"));
    auto steps = j::field(request, "steps");
    j::Value results(json_object_new_array());
    ct::SensorClock clock;
    ct::AlignmentGuide alignment;
    ct::KeyboardMode keyboard;
    ct::SteamShortcuts shortcuts;
    ct::PoseHistory history;
    ct::BindingCapture binding(j::find(request, "started") ? j::number(j::field(request, "started"))
                                                           : 0);
    if (auto warm = j::find(request, "warm")) {
        clock.scale = j::number(j::field(warm, "scale"));
        clock.offset = j::number(j::field(warm, "offset"));
    }
    for (size_t index = 0; index < j::array_size(steps); ++index) {
        auto step = j::item(steps, index);
        j::Value result;
        try {
            if (component == "settings") {
                result = ct::Settings::from_json(step).to_json();
            } else if (component == "bridge_input") {
                const auto state = j::field(step, "controls");
                ct::InputControls controls;
                controls.trigger = j::number(j::field(state, "trigger"));
                controls.grip = j::number(j::field(state, "grip"));
                controls.stick_x = j::number(j::field(state, "stick_x"));
                controls.stick_y = j::number(j::field(state, "stick_y"));
                controls.pad_x = j::number(j::field(state, "pad_x"));
                controls.pad_y = j::number(j::field(state, "pad_y"));
                controls.scroll_x = j::number(j::field(state, "scroll_x"));
                controls.scroll_y = j::number(j::field(state, "scroll_y"));
                controls.back = j::boolean(j::field(state, "back"));
                controls.bumper = j::boolean(j::field(state, "bumper"));
                controls.stick_touch = j::boolean(j::field(state, "stick_touch"));
                controls.stick_click = j::boolean(j::field(state, "stick_click"));
                controls.pad_touch = j::boolean(j::field(state, "pad_touch"));
                controls.pad_click = j::boolean(j::field(state, "pad_click"));
                controls.scroll_touch = j::boolean(j::field(state, "scroll_touch"));
                controls.scroll_click = j::boolean(j::field(state, "scroll_click"));
                std::optional<ct::BodyPoseMessage> pose;
                if (auto value = j::find(step, "pose"))
                    pose = ct::BodyPoseMessage{j::number(j::field(value, "time")), 0,
                                               j::numbers<3>(j::field(value, "position")), 0,
                                               j::numbers<4>(j::field(value, "orientation"))};
                const auto command = ct::bridge_input(
                    j::number(j::field(step, "session_start")), j::number(j::field(step, "time")),
                    pose, controls, j::boolean(j::field(step, "enabled")));
                result.reset(json_object_new_string(
                    hex(reinterpret_cast<const uint8_t *>(&command), sizeof(command)).c_str()));
            } else if (component == "report") {
                const auto data = bytes(step);
                result.reset(encode(ct::decode_report(data)));
            } else if (component == "serial") {
                const auto data = bytes(step);
                result.reset(
                    json_object_new_string(ct::parse_serial(data.data(), data.size()).c_str()));
            } else if (component == "setting") {
                const auto setting = j::integer(j::field(step, "setting")),
                           value = j::integer(j::field(step, "value"));
                if (setting < 0 || setting > UINT32_MAX || value < 0 || value > 65535)
                    throw std::invalid_argument("unsupported transient setting");
                const auto data = ct::setting_report(uint32_t(setting), uint32_t(value));
                result.reset(json_object_new_string(hex(data.data(), data.size()).c_str()));
            } else if (component == "clock") {
                const auto tick = j::integer(j::field(step, "tick"));
                const auto modulus = j::find(step, "modulus")
                                         ? j::integer(j::field(step, "modulus"))
                                         : int64_t(uint64_t(1) << 32);
                const auto value = clock.add(uint64_t(tick), j::number(j::field(step, "received")),
                                             uint64_t(modulus));
                result.reset(json_object_new_object());
                set(result.get(), "value", value);
                set(result.get(), "scale", clock.scale);
                set(result.get(), "offset", clock.offset);
                set(result.get(), "elapsed", json_object_new_int64(clock.elapsed));
            } else if (component == "orientation") {
                const auto op = j::string(j::field(step, "op"));
                const auto q = j::numbers<4>(j::field(step, "q"));
                if (op == "integrate")
                    result.reset(j::value(ct::integrate(q, j::numbers<3>(j::field(step, "angular")),
                                                        j::number(j::field(step, "dt")))));
                else if (op == "rotate")
                    result.reset(j::value(ct::rotate(q, j::numbers<3>(j::field(step, "vector")))));
                else if (op == "multiply")
                    result.reset(j::value(ct::multiply(q, j::numbers<4>(j::field(step, "other")))));
                else
                    throw std::invalid_argument("unknown orientation operation");
            } else if (component == "geometry") {
                const auto op = j::string(j::field(step, "op"));
                if (op == "axes")
                    result.reset(j::value(ct::axes_transform(
                        j::numbers<3>(j::field(step, "x")), j::numbers<3>(j::field(step, "z")),
                        j::numbers<3>(j::field(step, "position")))));
                else {
                    const auto pose = j::numbers<12>(j::field(step, "pose"));
                    if (op == "inverse")
                        result.reset(j::value(ct::inverse_pose(pose)));
                    else if (op == "quaternion")
                        result.reset(j::value(ct::quaternion(pose)));
                    else if (op == "transform")
                        result.reset(j::value(
                            ct::transform_point(pose, j::numbers<3>(j::field(step, "point")))));
                    else
                        throw std::invalid_argument("unknown geometry operation");
                }
            } else if (component == "history") {
                const auto op = j::string(j::field(step, "op"));
                const auto time = j::number(j::field(step, "time"));
                if (op == "add") {
                    auto pose = j::field(step, "pose");
                    history.add(time, pose ? std::optional<ct::RigidPose>(j::numbers<12>(pose))
                                           : std::nullopt);
                } else {
                    const auto value = history.at(time);
                    if (value)
                        result.reset(j::value(*value));
                }
            } else if (component == "alignment") {
                const auto now = j::number(j::field(step, "now"));
                if (auto begin = j::find(step, "begin"))
                    alignment.begin(j::number(begin));
                if (auto finish = j::find(step, "finish"))
                    alignment.finish(j::number(finish));
                const auto action =
                    alignment.update(buttons(j::field(step, "buttons")),
                                     j::optional_number(j::field(step, "received")), now);
                result.reset(json_object_new_object());
                set(result.get(), "action",
                    action == ct::AlignmentAction::None
                        ? nullptr
                        : json_object_new_string(
                              action == ct::AlignmentAction::Heading ? "heading" : "restart"));
                set(result.get(), "progress", alignment.progress(now));
                set(result.get(), "started", alignment.started);
                set(result.get(), "completed", alignment.completed);
            } else if (component == "keyboard") {
                const auto now = j::number(j::field(step, "now"));
                if (auto event = j::find(step, "observe"))
                    keyboard.observe({j::number(j::field(event, "time")),
                                      j::boolean(j::field(event, "visible")),
                                      j::boolean(j::field(event, "gaze_valid")),
                                      j::boolean(j::field(event, "gaze_on_keyboard"))},
                                     now);
                std::optional<ct::ControllerInput> input;
                if (auto state = j::find(step, "input")) {
                    input.emplace();
                    input->received = j::number(j::field(state, "received"));
                    input->report.buttons = buttons(j::field(state, "buttons"));
                    input->report.triggers = j::numbers<2>(j::field(state, "triggers"));
                }
                const auto typing = keyboard.update(input, j::boolean(j::field(step, "enabled")),
                                                    j::number(j::field(step, "received")), now);
                result.reset(json_object_new_object());
                set(result.get(), "typing", typing);
                set(result.get(), "contact", keyboard.contact);
                set(result.get(), "released_at", keyboard.released_at);
            } else if (component == "shortcuts") {
                const auto now = j::number(j::field(step, "now"));
                const auto actions = shortcuts.update(buttons(j::field(step, "buttons")), now,
                                                      j::boolean(j::field(step, "enabled")),
                                                      j::boolean(j::field(step, "keyboard")));
                result.reset(json_object_new_object());
                auto array = json_object_new_array();
                for (const auto &action : actions)
                    json_object_array_add(array, json_object_new_string(action.c_str()));
                set(result.get(), "actions", array);
                set(result.get(), "progress", shortcuts.progress(now));
            } else if (component == "binding") {
                const auto capture =
                    binding.update(j::field(step, "buttons")
                                       ? std::optional<uint32_t>(buttons(j::field(step, "buttons")))
                                       : std::nullopt,
                                   j::number(j::field(step, "now")));
                if (capture)
                    result.reset(json_object_new_int64(*capture));
            } else if (component == "labels") {
                result.reset(json_object_new_string(ct::binding_label(buttons(step)).c_str()));
            } else
                throw std::invalid_argument("unknown replay component");
        } catch (const std::exception &error) {
            result.reset(json_object_new_object());
            set(result.get(), "error", std::string(error.what()));
        }
        json_object_array_add(results.get(), result.release());
    }
    return results;
}
} // namespace
int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            auto request = j::parse(line, 8 * 1024 * 1024);
            auto result = replay(request.get());
            std::cout << json_object_to_json_string_ext(result.get(), JSON_C_TO_STRING_PLAIN)
                      << '\n';
        } catch (const std::exception &error) {
            j::Value result(json_object_new_object());
            set(result.get(), "error", std::string(error.what()));
            std::cout << json_object_to_json_string_ext(result.get(), JSON_C_TO_STRING_PLAIN)
                      << '\n';
        }
    }
}
