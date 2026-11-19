#include "helper/acquisition_reader.h"
#include "acquisition_replay.h"
#include "app_replay.h"
#include "download_replay.h"
#include "update_client_replay.h"
#include "app/settings.h"
#include "helper/patch.h"
#include "controller/report.h"
#include "controller/sensor_clock.h"
#include "interaction/gestures.h"
#include "platform/json.h"
#include "steamvr/codec.h"
#include "steamvr/steam_ui_transport.h"
#include "tracking/acquisition.h"
#include "tracking/math.h"
#include "tracking/native_pose.h"
#include "tracking/position.h"
#include "input_control_replay.h"
#include "session_replay.h"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <libunwind.h>

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
json_object *encode(const std::optional<ct::BodyPoseMessage> &pose) {
    if (!pose)
        return nullptr;
    auto result = json_object_new_object();
    set(result, "op", std::string("native_pose"));
    set(result, "time", json_object_new_double(pose->time));
    set(result, "position_time", json_object_new_double(pose->position_time));
    set(result, "position", j::value(pose->position));
    set(result, "orientation_time", json_object_new_double(pose->orientation_time));
    set(result, "orientation", j::value(pose->orientation));
    return result;
}
json_object *encode(const std::optional<ct::NativeObservation> &pose) {
    if (!pose)
        return nullptr;
    auto result = json_object_new_object();
    set(result, "time", json_object_new_double(pose->time));
    set(result, "position", j::value(pose->position));
    set(result, "orientation", j::value(pose->orientation));
    set(result, "native_orientation", j::value(pose->native_orientation));
    set(result, "emitter", j::value(pose->emitter));
    return result;
}
ct::ControllerReport imu_report(json_object *state) {
    ct::ControllerReport result;
    result.sensor_tick = uint64_t(j::integer(j::field(state, "sensor_tick")));
    if (auto modulus = j::find(state, "clock_modulus"))
        result.clock_modulus = uint64_t(j::integer(modulus));
    result.acceleration = j::numbers<3>(j::field(state, "acceleration"));
    result.angular_velocity = j::numbers<3>(j::field(state, "angular_velocity"));
    return result;
}
ct::BodyCalibration calibration(json_object *value) {
    ct::BodyCalibration result;
    result.schema = unsigned(j::integer(j::field(value, "schema")));
    result.controller_serial = j::string(j::field(value, "controller_serial"));
    auto mount = j::field(value, "body_from_report_rotation");
    if (j::array_size(mount) != 3)
        throw std::invalid_argument("invalid IMU body rotation");
    for (size_t i = 0; i < 3; ++i)
        result.body_from_report_rotation[i] = j::numbers<3>(j::item(mount, i));
    result.gyro_bias_report_rad_s = j::numbers<3>(j::field(value, "gyro_bias_report_rad_s"));
    return result;
}

j::Value replay(json_object *request) {
    if (j::string(j::field(request, "component")) == "download")
        return download_replay::replay(request);
    if (j::string(j::field(request, "component")) == "update_client")
        return update_client_replay::replay(request);
    if (j::string(j::field(request, "component")) == "application")
        return app_replay::replay(request);
    if (j::string(j::field(request, "component")) == "acquisition_memory")
        return acquisition_replay::replay(request);
    if (j::string(j::field(request, "component")) == "session")
        return session_replay::replay(request);
    if (j::string(j::field(request, "component")) == "input_control")
        return input_replay::replay(request);
    if (j::string(j::field(request, "component")) == "steam_ui_transport") {
        const auto before = update_client_replay::descriptors();
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
        set(result.get(), "fds_after", json_object_new_uint64(update_client_replay::descriptors()));
        return result;
    }
    const auto component = j::string(j::field(request, "component"));
    auto steps = j::field(request, "steps");
    j::Value results(json_object_new_array());
    ct::SensorClock clock;
    ct::AlignmentGuide alignment;
    ct::KeyboardMode keyboard;
    ct::SteamShortcuts shortcuts;
    ct::InitialAcquisition acquisition;
    ct::PoseHistory history;
    ct::InertialPosition position(
        j::find(request, "smoothing") ? j::number(j::field(request, "smoothing")) : .04);
    std::optional<ct::NativeBodyPose> native;
    if (component == "native_pose")
        native.emplace(calibration(j::field(request, "calibration")),
                       j::string(j::field(request, "serial")),
                       j::numbers<3>(j::field(request, "emitter")), position.smoothing);
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
            if (component == "gate") {
                const auto kind = j::string(j::field(step, "kind")) == "input"
                                      ? ct::helper::GateKind::Input
                                      : ct::helper::GateKind::Beam;
                const auto layout = ct::helper::place_gate(
                    kind, uintptr_t(j::unsigned_integer(j::field(step, "site"))),
                    uintptr_t(j::unsigned_integer(j::field(step, "cave"))));
                const auto op = j::string(j::field(step, "op"));
                if (op == "layout") {
                    result.reset(json_object_new_object());
                    set(result.get(), "site", json_object_new_uint64(layout.site));
                    set(result.get(), "cave", json_object_new_uint64(layout.cave));
                    set(result.get(), "index_offset", json_object_new_uint64(layout.index_offset));
                    set(result.get(), "index_limit", json_object_new_int(layout.index_limit));
                    for (const auto &[name, value] :
                         std::vector<std::pair<const char *, ct::helper::Bytes>>{
                             {"original", layout.original},
                             {"following", layout.following},
                             {"redirect", layout.redirect},
                             {"payload", layout.payload}})
                        set(result.get(), name, hex(value.data(), value.size()));
                } else if (op == "owned")
                    result.reset(json_object_new_boolean(
                        ct::helper::owned_gate(layout, bytes(j::field(step, "value")))));
                else if (op == "pc") {
                    const auto pc = ct::helper::gate_resume_pc(
                        layout, uintptr_t(j::unsigned_integer(j::field(step, "base"))),
                        uintptr_t(j::unsigned_integer(j::field(step, "pc"))));
                    if (pc)
                        result.reset(json_object_new_uint64(*pc));
                } else
                    throw std::invalid_argument("unknown gate test action");
            } else if (component == "acquisition_quality") {
                const auto state = ct::helper::acquisition_quality(step);
                const auto good = state.ready();
                const auto ready =
                    state.time ? acquisition.update(*state.time, good) : acquisition.ready;
                result.reset(json_object_new_object());
                set(result.get(), "good", good);
                set(result.get(), "ready", ready);
                set(result.get(), "since", acquisition.since);
                set(result.get(), "latest", acquisition.latest);
            } else if (component == "settings") {
                result = ct::Settings::from_json(step).to_json();
            } else if (component == "profile") {
                result = j::parse(ct::controller_profile(j::string(step)));
            } else if (component == "bridge_event") {
                const auto command = ct::bridge_event(
                    j::boolean(j::field(step, "connected")),
                    j::unsigned_integer(j::field(step, "hardware_id")),
                    j::string(j::field(step, "onboard")), j::string(j::field(step, "default")));
                result.reset(json_object_new_string(
                    hex(reinterpret_cast<const uint8_t *>(&command), sizeof(command)).c_str()));
            } else if (component == "bridge_imu") {
                const auto command =
                    ct::bridge_imu(j::number(j::field(step, "time")),
                                   j::numbers<3>(j::field(step, "acceleration")),
                                   j::numbers<3>(j::field(step, "angular_velocity")));
                result.reset(json_object_new_string(
                    hex(reinterpret_cast<const uint8_t *>(&command), sizeof(command)).c_str()));
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
            } else if (component == "decode_pose") {
                const auto data = bytes(step);
                const auto pose = ct::decode_pose(data.data(), data.size());
                result.reset(json_object_new_object());
                set(result.get(), "controller_id", json_object_new_int64(pose.controller_id));
                set(result.get(), "timestamp", json_object_new_double(pose.timestamp));
                set(result.get(), "valid", pose.valid);
                if (pose.valid) {
                    set(result.get(), "quaternion", j::value(*pose.orientation));
                    set(result.get(), "position", j::value(*pose.position));
                    set(result.get(), "velocity", j::value(pose.velocity));
                    set(result.get(), "angular_velocity", j::value(pose.angular_velocity));
                    set(result.get(), "acceleration", j::value(pose.acceleration));
                }
            } else if (component == "decode_status") {
                const auto data = bytes(step);
                const auto status = ct::decode_status(data.data(), data.size());
                result.reset(json_object_new_object());
                set(result.get(), "magic", json_object_new_int64(status.magic));
                set(result.get(), "version", json_object_new_int64(status.version));
                set(result.get(), "connected", json_object_new_int64(status.connected));
                set(result.get(), "error", json_object_new_int64(status.error));
                set(result.get(), "last_kind", json_object_new_int64(status.last_kind));
                set(result.get(), "input_count", json_object_new_int64(status.input_count));
                set(result.get(), "imu_count", json_object_new_int64(status.imu_count));
                set(result.get(), "dropped", json_object_new_int64(status.dropped));
                set(result.get(), "last_input", json_object_new_double(status.last_input));
                set(result.get(), "last_pose", json_object_new_double(status.last_pose));
                set(result.get(), "session_start", json_object_new_double(status.session_start));
                set(result.get(), "native_ready", json_object_new_int64(status.native_ready));
                set(result.get(), "pose_controller_id",
                    json_object_new_int64(status.pose_controller_id));
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
            } else if (component == "position") {
                const auto op = j::string(j::field(step, "op"));
                result.reset(json_object_new_object());
                if (op == "sample")
                    position.sample(j::number(j::field(step, "time")),
                                    j::numbers<3>(j::field(step, "acceleration")),
                                    j::number(j::field(step, "angular_speed")));
                else if (op == "observe")
                    set(result.get(), "accepted",
                        position.observe(j::number(j::field(step, "time")),
                                         j::numbers<3>(j::field(step, "position")),
                                         j::number(j::field(step, "received")),
                                         j::find(step, "confidence")
                                             ? j::number(j::field(step, "confidence"))
                                             : 1) != ct::InertialPosition::Fix::Rejected);
                else
                    throw std::invalid_argument("unknown position operation");
                const auto value = position.value(j::number(j::field(step, "now")));
                set(result.get(), "value", value ? j::value(*value) : nullptr);
                set(result.get(), "velocity",
                    position.velocity ? j::value(*position.velocity) : nullptr);
                set(result.get(), "stationary", position.stationary);
                set(result.get(), "history_size", json_object_new_int64(position.history.size()));
                set(result.get(), "observation_size",
                    json_object_new_int64(position.observations.size()));
            } else if (component == "native_pose") {
                const auto op = j::string(j::field(step, "op"));
                const auto now = j::number(j::field(step, "now"));
                result.reset(json_object_new_object());
                if (op == "sample") {
                    const auto head = j::find(step, "head");
                    set(result.get(), "result",
                        native->sample(imu_report(j::field(step, "imu")),
                                       j::number(j::field(step, "received")),
                                       j::number(j::field(step, "frame_offset")),
                                       head ? std::optional<ct::RigidPose>(j::numbers<12>(head))
                                            : std::nullopt));
                } else if (op == "observe") {
                    const auto pose = j::field(step, "pose");
                    ct::NativePoseInput input;
                    if (auto id = j::find(pose, "controller_id"))
                        input.controller_id = uint32_t(j::integer(id));
                    if (auto time = j::find(pose, "timestamp"))
                        input.timestamp = j::number(time);
                    if (auto valid = j::find(pose, "valid"))
                        input.valid = j::boolean(valid);
                    if (auto q = j::find(pose, "quaternion"))
                        input.orientation = j::numbers<4>(q);
                    if (auto p = j::find(pose, "position"))
                        input.position = j::numbers<3>(p);
                    set(result.get(), "result", native->observe(input, now));
                } else if (op == "align")
                    native->align_head(now, j::numbers<12>(j::field(step, "head")));
                else if (op == "recenter")
                    native->recenter();
                else if (op == "heading")
                    native->apply_heading(j::number(j::field(step, "yaw")), now);
                else if (op != "inspect")
                    throw std::invalid_argument("unknown native pose operation");
                if (op == "inspect" ||
                    (j::find(step, "inspect") && j::boolean(j::field(step, "inspect")))) {
                    set(result.get(), "message", encode(native->message(now)));
                    auto input = encode(native->input_pose(now));
                    if (input) {
                        json_object_object_del(input, "op");
                        json_object_object_del(input, "position_time");
                        json_object_object_del(input, "orientation_time");
                    }
                    set(result.get(), "input_pose", input);
                    set(result.get(), "strict_native", encode(native->strict_native(now)));
                    set(result.get(), "palm_aligned", native->palm_aligned);
                    set(result.get(), "imu_recoveries",
                        json_object_new_int(native->imu_recoveries));
                    set(result.get(), "rejected_poses",
                        json_object_new_int(native->rejected_poses));
                    set(result.get(), "orientation",
                        native->orientation ? j::value(*native->orientation) : nullptr);
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
            } else if (component == "readiness") {
                const auto ready = acquisition.update(j::number(j::field(step, "time")),
                                                      j::boolean(j::field(step, "good")));
                result.reset(json_object_new_object());
                set(result.get(), "ready", ready);
                set(result.get(), "since", acquisition.since);
                set(result.get(), "latest", acquisition.latest);
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
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--update-download-worker")
        return ct::update::download_worker();
    // libunwind keeps a process-wide probe pipe. Initialize it before descriptor checks.
    void *frames[32];
    if (unw_backtrace(frames, 32) <= 0)
        return 1;
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
