#include "controller/report.h"
#include "controller/sensor_clock.h"
#include "helper/acquisition_reader.h"
#include "interaction/gestures.h"
#include "input_control_replay.h"
#include "platform/json.h"
#include "steamvr/codec.h"
#include "tracking/acquisition.h"
#include "tracking/math.h"
#include "tracking/native_pose.h"
#include "tracking/position.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace
int main() {
    using namespace ct;
    {
        input_replay::Backend backend;
        InputControl control(backend, "TEST1234");
        control.state.enabled = control.state.native_active = true;
        control.update_beam(100);
        assert(!control.state.activation_confirmed);
        backend.selected = true;
        backend.device.reset();
        control.update_beam(100.01);
        assert(!control.state.activation_confirmed);
        backend.device = 3;
        control.update_beam(100.02);
        assert(control.state.activation_confirmed);
        backend.selected = false;
        control.update_beam(100.03);
        assert(control.state.activation_confirmed);
        control.controls(std::nullopt, false);
        assert(!control.state.activation_confirmed && !control.state.native_active);
        control.state.enabled = control.state.native_active = true;
        control.update_beam(100.04);
        assert(!control.state.activation_confirmed);
        backend.selected = true;
        control.update_beam(100.05);
        assert(control.state.activation_confirmed);
    }
    {
        SteamShortcuts keys;
        keys.update(0, 10, true, true);
        keys.update(0x10000, 10.01, true, true);
        keys.update(0x10000, 10.2, true, true);
        keys.update(0x10000, 10.4, true, true);
        assert(!keys.progress(10.50));
        keys.update(0x10000, 10.52, true, true);
        assert(keys.progress(10.52));
        assert(keys.update(0x10004, 10.55, true, true) == std::vector<std::string>{"keyboard"});
        assert(!keys.progress(10.55));
        assert(keys.update(0, 10.6, true, true).empty());
        for (double now = 10.8; now < 11.5; now += .1)
            assert(keys.update(0, now, true, true).empty());
        keys.update(0x10000, 11.5, true, true);
        assert(keys.update(0, 11.6, true, true).empty());
        for (double now : {11.8, 12.0})
            assert(keys.update(0, now, true, true).empty());
        assert(keys.update(0, 12.11, true, true) == std::vector<std::string>{"dashboard"});
        keys.update(0x10000, 12.2, true, true);
        keys.update(0, 12.25, true, true);
        keys.update(0x10000, 12.35, true, true);
        assert(keys.update(0, 12.4, true, true) == std::vector<std::string>{"passthrough"});
        for (double now : {12.6, 12.8, 13., 13.2})
            assert(keys.update(0, now, true, true).empty());
        keys.update(0x10000, 13.3, true, true);
        for (double now = 13.4; now < 14.4; now += .1)
            keys.update(0x10000, now, true, true);
        assert(keys.update(0, 14.4, true, true) == std::vector<std::string>{"recenter"});
        keys.update(0x10000, 14.5, false, true);
        assert(keys.update(0, 14.6, false, true) == std::vector<std::string>{"dashboard"});
    }
    for (uint32_t first : {0x10000u, 0x200u, 0x10200u}) {
        SteamShortcuts keys;
        assert(keys.update(0, 10, true, true).empty());
        const auto initial = keys.update(first, 10.01, true, true);
        assert(initial == (first == 0x10200 ? std::vector<std::string>{"screenshot"}
                                            : std::vector<std::string>{}));
        const auto chord = keys.update(0x10200, 10.02, true, true);
        assert(chord == (first == 0x10200 ? std::vector<std::string>{}
                                          : std::vector<std::string>{"screenshot"}));
        for (double now = 10.1; now < 11.5; now += .1) {
            assert(keys.update(0x10200, now, true, true).empty());
            assert(!keys.progress(now));
        }
        assert(keys.update(0x10000, 11.5, true, true).empty());
        assert(keys.update(0x10200, 11.51, true, true) == std::vector<std::string>{"screenshot"});
        assert(keys.update(0, 11.6, true, true).empty());
        for (double now = 11.7; now < 12.3; now += .1)
            assert(keys.update(0, now, true, true).empty());
    }
    {
        SteamShortcuts keys;
        constexpr uint32_t Steam = 0x10000, Camera = 0x90000, Screenshot = 0x10200;
        assert(keys.update(Camera, 10, true, true).empty());
        assert(keys.update(0, 10.01, true, true).empty());
        assert(keys.update(Steam, 10.02, true, true).empty());
        assert(keys.update(Camera, 10.03, true, true) == std::vector<std::string>{"camera_switch"});
        assert(keys.update(Camera, 10.04, true, true).empty());
        assert(!keys.progress(10.04));
        assert(keys.update(0, 10.20, false, false, false).empty());
        assert(keys.update(Camera, 10.21, true, true).empty());
        assert(keys.update(0, 10.22, true, true).empty());
        assert(keys.update(Camera | Screenshot, 10.23, true, true) ==
               std::vector<std::string>{"screenshot"});
        assert(keys.update(Camera, 10.24, true, true).empty());
        assert(keys.update(0, 10.25, true, true).empty());
        assert(keys.update(Camera, 10.26, true, true) == std::vector<std::string>{"camera_switch"});
        assert(keys.update(0, 10.27, true, true).empty());
    }
    for (bool keyboard : {false, true}) {
        SteamShortcuts keys;
        assert(keys.update(0x10200, 10, true, keyboard).empty());
        assert(keys.update(0, 10.1, true, keyboard).empty());
        assert(keys.update(0x10200, 11, true, keyboard).empty());
        assert(keys.update(0, 11.1, false, keyboard).empty());
        assert(keys.update(0x10200, 11.2, false, keyboard).empty());
        assert(keys.update(0x10200, 11.3, true, keyboard).empty());
        assert(keys.update(0x10000, 11.4, true, keyboard).empty());
        assert(keys.update(0x10200, 11.5, true, keyboard) ==
               std::vector<std::string>{"screenshot"});
    }
    {
        helper::AcquisitionReader short_reader(
            [](uintptr_t, size_t size) { return helper::Bytes(size - 1); });
        rejects([&] { short_reader.record(8); });
        rejects([&] { short_reader.feature_group(8); });
        rejects([&] { short_reader.palm_estimator(8); });
        unsigned reads = 0;
        helper::AcquisitionReader overflow([&](uintptr_t address, size_t size) {
            ++reads;
            const uint64_t value = address == helper::AcquisitionReader::AppGlobal
                                       ? UINT64_MAX - 255
                                       : helper::AcquisitionReader::AppVtable;
            assert(size == 8);
            helper::Bytes bytes(8);
            std::memcpy(bytes.data(), &value, 8);
            return bytes;
        });
        rejects([&] { overflow.snapshot(); });
        assert(reads == 2);
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    AcquisitionQuality quality;
    quality.time = 10;
    quality.slots = {{0, true, 1, 0, 1, 9.9}};
    quality.palms = {{1, true, {0, 0, 0, 0, 0, 0}}};
    quality.groups = {{0, {{6, 6, 0}, {6, 6, 30}}}};
    assert(quality.ready());
    quality.palms[0].covariance[0] = nan;
    assert(!quality.ready());
    quality.palms[0].covariance[0] = 0;
    quality.groups[0].cameras[0].rms = infinity;
    assert(!quality.ready());
    quality.groups[0].cameras[0].rms = 0;
    quality.time = nan;
    assert(!quality.ready());
    static_assert(ButtonMask == 0x0ccfffff);
    rejects([&] { decode_report(nullptr, 0); });
    rejects([&] { setting_report(0x24, 101); });
    rejects([&] { setting_report(0x30, 1); });
    rejects([&] { setting_report(0x124, 0); });
    assert(setting_report(0x24, 100)[4] == 100);
    assert(setting_report(0x30, 0x18)[4] == 0x18);
    rejects([&] { integrate({1, 0, 0, 0}, {}, nan); });
    rejects([&] { integrate({nan, 0, 0, 0}, {}, .01); });
    rejects([&] { integrate({1, 0, 0, 0}, {infinity, 0, 0}, .01); });
    rejects([&] { SensorClock{}.add(0, nan); });
    rejects([&] { SensorClock{}.add(0, 1, 128); });
    PoseHistory history;
    rejects([&] { history.at(infinity); });
    rejects([&] { history.add(nan, std::nullopt); });
    rejects([&] { KeyboardMode{}.observe({nan, true, true, true}, 1); });
    rejects([&] { KeyboardMode{}.observe({1, false, true, true}, 1); });
    InertialPosition position;
    rejects([&] { position.sample(1, {100, 0, 0}, 0); });
    rejects([&] { position.sample(1, {}, nan); });
    rejects([&] { position.observe(1, {nan, 0, 0}, 1); });
    rejects([&] { json::parse("{\"valid\":true}junk"); });
    rejects([&] { json::parse(std::string("{\"text\":\"") + char(0xff) + "\"}"); });
    rejects([&] { json::parse("{}", 1); });
    auto json_value = json::parse("{\"number\":true,\"flag\":1}");
    rejects([&] { json::number(json::field(json_value.get(), "number")); });
    rejects([&] { json::boolean(json::field(json_value.get(), "flag")); });
    rejects([&] { json::field(json_value.get(), "missing"); });
    assert(json::encode(json_value.get()) == R"({"number":true,"flag":1})");
    rejects([&] { json::encode(json_value.get(), 1); });
    json_object_object_add(json_value.get(), "number", json_object_new_double(infinity));
    rejects([&] { json::encode(json_value.get()); });
    auto nested = json::parse("[]");
    auto parent = nested.get();
    for (unsigned i = 0; i < 65; ++i) {
        auto child = json_object_new_array();
        json_object_array_add(parent, child);
        parent = child;
    }
    rejects([&] { json::encode(nested.get()); });
    rejects([&] { bridge_imu(nan, {}, {}); });
    rejects([&] { bridge_imu(1, {1e100, 0, 0}, {}); });
    rejects([&] { bridge_input(0, 1, std::nullopt, {}, false); });
    rejects([&] { bridge_event(true, 0x47, "{}", "{}"); });
    rejects([&] { bridge_event(true, 0x47, std::string(12288, ' '), "{}"); });
    const auto profile = controller_profile("TEST1234");
    auto connection = bridge_event(true, 0x47, profile, profile);
    std::string serial;
    assert(wire::event(connection, serial));
    assert(serial == "controllerutils-sc26-TEST1234");
    auto imu = bridge_imu(10, {0, 9.80665, 0}, {});
    assert(wire::imu(imu, 10, 9));
    auto input = bridge_input(10, 10.1, std::nullopt, {}, false);
    assert(wire::app_input(input, 10.1, 0, 10));
    InputControls confirmed;
    confirmed.activate = false;
    input = bridge_input(10, 10.1, std::nullopt, confirmed, true);
    assert(wire::app_input(input, 10.1, 0, 10));
    wire::AppInput selected{};
    std::memcpy(&selected, input.data, sizeof(selected));
    assert(selected.flags == 5);
    for (auto flags : {4u, 8u}) {
        selected.flags = flags;
        std::memcpy(input.data, &selected, sizeof(selected));
        assert(!wire::app_input(input, 10.1, 0, 10));
    }
    wire::PoseBlock pose{};
    pose.timestamp = -1;
    pose.orientation[0] = nan;
    assert(!decode_pose(reinterpret_cast<const uint8_t *>(&pose), sizeof(pose)).valid);
    pose.timestamp = 1;
    rejects([&] { decode_pose(reinterpret_cast<const uint8_t *>(&pose), sizeof(pose)); });
    BodyCalibration calibration{1, "TEST1234", {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {}};
    rejects([&] { NativeBodyPose(calibration, "OTHER", {}, .04); });
    rejects([&] { NativeBodyPose(calibration, "TEST1234", {}, nan); });
    NativeBodyPose tracker(calibration, "TEST1234", {0, .01, -.02});
    assert(!tracker.message(10));
    assert(!tracker.observe({16, nan, true, std::nullopt, std::nullopt}, 10));
    assert(!tracker.observe({16, 10, true, Quaternion{nan, 0, 0, 0}, Vector3{}}, 10));
    assert(tracker.rejected_poses == 1);
    assert(position.observe(10, {}, 10) == InertialPosition::Fix::Rejected);
    for (int i = 0; i < 10000; ++i) {
        const double time = 10 + i * .01;
        position.sample(time, {}, 0);
        if (i > 6 && i % 3 == 0)
            position.observe(time - .02, {0, 0, -.4}, time);
    }
    assert(position.history.size() <= 512 && position.observations.size() <= 96);
    assert(distance(*position.value(109.99), {0, 0, -.4}) < 1e-9);

    NativeBodyPose stability(calibration, "TEST1234", {});
    const auto head = rigid_pose({1, 0, 0, 0}, {});
    const auto sample = [&](int tick, double force = 0, double yaw = 0) {
        const double now = 20 + tick * .005;
        ControllerReport report;
        report.sensor_tick = tick * 5000;
        report.acceleration = {force, 9.80665, 0};
        report.angular_velocity = {0, yaw, 0};
        stability.sample(report, now, 0, head);
        return now;
    };
    const auto observe = [&](double now, double x, double age = .02) {
        return stability.observe({16, now - age, true, Quaternion{1, 0, 0, 0}, Vector3{x, 0, .4}},
                                 now);
    };
    for (int tick = 0; tick <= 320; ++tick) {
        const double now = sample(tick);
        if (tick >= 270)
            assert(observe(now, 0));
    }
    const auto trusted = *stability.message(21.6);
    assert(stability.motion.stationary_anchor);
    {
        auto parked = stability;
        parked.apply_heading(0, 21.6);
        const Vector3 grip_offset{-.07, .02, 0};
        const auto tick = [&](NativeBodyPose &body, int i, bool moving = false) {
            const double now = 21.6 + i * .01;
            ControllerReport report;
            report.sensor_tick = 1600000 + i * 10000;
            report.acceleration = {0, 9.80665, 0};
            report.angular_velocity = {0, moving ? .18 : 0, 0};
            body.sample(report, now, 0, head);
            return now;
        };
        const auto native_at = [&](NativeBodyPose &body, double now, double x) {
            return body.observe({16, now - .02, true, Quaternion{1, 0, 0, 0}, Vector3{x, 0, .4}},
                                now);
        };
        const auto palm_at = [&](NativeBodyPose &body, double now, double stamp, double x,
                                 size_t side = 0) {
            const auto pose = body.rotation->history.at(stamp);
            assert(pose);
            PalmPoints palms{};
            palms[side] = add(Vector3{x, 0, -.4}, rotate(quaternion(*pose), grip_offset));
            std::array<double, 2> stamps{};
            stamps[side] = stamp;
            body.observe_hands(stamps, palms, now);
        };
        const auto path = [](double stamp) { return std::clamp((stamp - 21.6) * .2, 0., .10); };
        for (int i = 1; i <= 100; ++i) {
            const auto t = tick(parked, i, i <= 50);
            if (i >= 4)
                assert(native_at(parked, t, path(t - .02)));
            if (i >= 10 && i % 5 == 0)
                palm_at(parked, t, t - .07, path(t - .07));
        }
        assert(parked.motion.stationary_anchor &&
               parked.hands.grip_center(0, 22.6, {}, {1, 0, 0, 0}));
        for (int i = 101; i <= 700; ++i) {
            const auto t = tick(parked, i);
            assert(!native_at(parked, t, .24));
            if (i % 10 == 0) {
                if (i < 400)
                    palm_at(parked, t, t - .07, .24, 1); // A different hand beside the candidate.
                else
                    palm_at(parked, t, t - .07, .10); // The gripping hand still at the anchor.
            }
        }
        assert(!parked.input_pose(28.6));
        for (double x : {.145, .24}) {
            auto body = parked;
            for (int i = 701; i <= 760; ++i) {
                const auto t = tick(body, i);
                const auto before = body.message(t)->position;
                const bool old_fix = i == 732 || i == 734;
                const bool accepted = native_at(body, t, old_fix ? .10 : x);
                if (i == 717 || i == 727)
                    palm_at(body, t, t - .07, x);
                if (i > 717 && i < 727)
                    palm_at(body, t, 21.6 + 710 * .01, x); // Repeated camera frame.
                assert(accepted == (i >= 728 && !old_fix));
                if (i == 728) {
                    assert(body.strict_native(t) && body.input_pose(t));
                    assert(distance(body.message(t)->position, before) < 1e-8);
                }
                if (i == 736)
                    assert(body.message(t)->position[0] > .105 && body.message(t)->position[0] < x);
            }
            assert(body.motion.stationary_anchor &&
                   std::abs((*body.motion.stationary_anchor)[0] - x) < 1e-9);
            assert(distance(body.message(29.2)->position, {x, 0, -.4}) < .002);
        }
        auto rebased = parked;
        for (int i = 701; i <= 730; ++i) {
            const auto t = tick(rebased, i);
            assert(!native_at(rebased, t, .24));
            if (i == 717 || i == 727)
                palm_at(rebased, t, t - .07, .24);
            if (i == 727)
                rebased.reset_hands();
        }
        assert(rebased.motion.stationary_anchor && !rebased.strict_native(28.9));
        // Switching native clusters discards the earlier palm vote. Stale palms cannot finish it.
        for (int i = 701; i <= 745; ++i) {
            const auto t = tick(parked, i);
            assert(!native_at(parked, t, i == 719 ? .35 : .24));
            if (i == 717 || i == 729)
                palm_at(parked, t, t - .07, .24);
            if (i == 744)
                palm_at(parked, t, t - .20, .24);
        }
        assert(parked.motion.stationary_anchor && !parked.strict_native(29.05));
    }
    const double spike = sample(321);
    assert(!observe(spike, .9));
    assert(stability.rejected_poses == 1 && stability.strict_native(spike)->time == trusted.time);
    assert(distance(stability.input_pose(spike)->position, trusted.position) < 1e-9);
    const double clock_skew = sample(322);
    assert(observe(clock_skew, 0, -.004));
    assert(distance(stability.message(clock_skew)->position, trusted.position) < 1e-9);

    // A slow sequence of fresh but false fixes must not walk the model away.
    for (int tick = 323; tick <= 520; ++tick) {
        const double now = sample(tick);
        if (tick >= 330)
            observe(now, .002 * (tick - 330));
        assert(distance(stability.message(now)->position, trusted.position) < .05);
    }
    assert(!stability.input_pose(22.6));

    // Real motion releases the stationary anchor. A false return still needs confirmation.
    const double moved = sample(521, 1.2);
    const auto held = *stability.message(moved);
    assert(!observe(moved, 1.4));
    for (int tick = 522; tick <= 529; ++tick) {
        const double now = sample(tick, 1.2);
        if (tick == 523 || tick == 526 || tick == 529)
            assert(observe(now, .65) == (tick == 529));
    }
    const double returned = 20 + 529 * .005;
    assert(stability.input_pose(returned));
    assert(distance(stability.message(returned)->position, held.position) < .01);
    for (int tick = 530; tick <= 549; ++tick) {
        const auto now = sample(tick, 1.2, .3);
        assert(observe(now, .65));
    }
    assert(stability.message(returned + .1)->position[0] < .58);
    for (int tick = 550; tick <= 1200; ++tick)
        sample(tick, 1.2, .3);
    const auto after_loss = *stability.message(26);
    assert(distance(after_loss.position, {.65, 0, -.4}) <= .05 + 1e-9);
    assert(!stability.input_pose(26) && stability.orientation_time == after_loss.orientation_time);
    assert(std::abs(after_loss.orientation[2]) > .1);

    // Rebuild IMU history after a clock gap without letting an unbracketed pose reset position.
    const auto resumed = sample(1220);
    assert(!observe(resumed, 1.8));
    assert(distance(stability.message(resumed)->position, after_loss.position) < 1e-9);

    NativeBodyPose waking(calibration, "TEST1234", {});
    const Vector3 seen{0, 0, -.4};
    const Quaternion identity{1, 0, 0, 0};
    // Learn the grip in controller coordinates. Cover the LED, turn and translate the pad.
    for (unsigned mask : {1u, 2u, 3u}) {
        HandPosition hands;
        double optical = 10;
        Vector3 display{}, base{};
        const auto palms = [&](const Vector3 &point, const Quaternion &q) {
            PalmPoints result;
            for (unsigned i = 0; i < 2; ++i)
                if (mask & (1u << i))
                    result[i] = add(point, rotate(q, {i ? .075 : -.075, 0, .02}));
            return result;
        };
        for (int i = 0; i < 12; ++i) {
            optical = 10 + i / 30.;
            const Vector3 visible{(i - 11) * .005, 0, 0};
            hands.observe(optical, optical, palms(visible, identity), identity, visible, visible,
                          true);
        }
        for (int i = 1; i <= 24; ++i) {
            const double now = 10 + (11 + i) / 30.;
            const Vector3 center{i * .005, 0, 0};
            const Quaternion q{std::cos(i * .014), 0, std::sin(i * .014), 0};
            display = hands.value(now, base);
            // Include a service which continues returning fresh, drifting positions.
            if (mask == 2) {
                optical = now;
                base = {-i * .003, 0, 0};
                hands.native_observed(optical, now, base, true);
            }
            hands.observe(now, now, palms(center, q), q,
                          mask == 2 ? std::optional<Vector3>(base) : std::nullopt, display, true);
            display = hands.value(now, base);
            if (i > 7) {
                assert(hands.recent(now));
                assert(distance(display, center) < .012);
            }
        }
        // A released grip cannot drive clicks. The display still blends back on a real fix.
        const double lost = 11.36;
        hands.observe(lost, lost, {}, identity, std::nullopt, display, false);
        assert(!hands.recent(lost));
        const auto last = hands.value(lost, base);
        const Vector3 returned{.16, 0, 0};
        hands.native_observed(12, 12, returned, false);
        assert(distance(hands.value(12, returned), last) < 1e-9);
        assert(distance(hands.value(12.08, returned), last) < 1e-9);
        for (unsigned i = 0; i <= 8; ++i)
            hands.native_observed(12.2 + i * .01, 12.2 + i * .01, returned, false);
        assert(distance(hands.value(12.32, returned), returned) < distance(last, returned));
        assert(hands.value(12.42, returned) == returned);
    }
    // Replay delayed palms at 10 Hz against a fresh but wrong native stream at 100 Hz.
    {
        HandPosition hands;
        const PalmPoints palm{Vector3{-.07, 0, 0}, std::nullopt};
        for (int i = 0; i <= 6; ++i) {
            const double time = 10 + i * .1;
            const Vector3 center{(i - 6) * .01, 0, 0};
            hands.observe(time, time + .08, {add(*palm[0], center), std::nullopt}, identity, center,
                          center, true);
        }
        const Vector3 wrong{.08, 0, 0};
        Vector3 previous = wrong;
        for (int i = 0; i < 110; ++i) {
            const double now = 10.69 + i * .01;
            hands.native_observed(now - .025, now, wrong, true);
            const auto before = hands.value(now, wrong);
            if (i % 10 == 0 && i != 50) {
                hands.observe(now - .08, now, palm, identity, wrong, before, true);
                assert(distance(hands.value(now, wrong), before) < 1e-9);
            }
            const auto point = hands.value(now, wrong);
            assert(point[0] <= previous[0] + 1e-9 && point[0] >= -1e-9);
            if (i >= 30)
                assert(norm(point) < .001);
            previous = point;
        }
        // The native stream rejoins the grip. A single near measurement followed by conflict
        // must not end assistance. Sustained agreement releases it without a jump.
        const double now = 11.8;
        hands.observe(now - .08, now, palm, identity, Vector3{}, hands.value(now, wrong), true);
        hands.observe(now + .02, now + .10, palm, identity, wrong, hands.value(now + .10, wrong),
                      true);
        assert(norm(hands.value(now + .19, wrong)) < .002);
        for (int i = 2; i <= 6; ++i) {
            const double received = now + i * .10;
            hands.native_observed(received - .025, received, {}, true);
            hands.observe(received - .08, received, palm, identity, Vector3{},
                          hands.value(received, {}), true);
        }
        assert(hands.value(now + .75, wrong) == wrong);
    }
    // Lose an enrolled, one-handed grip while native timestamps keep advancing.
    {
        HandPosition enrolled;
        const Vector3 offset{-.07, 0, 0};
        for (int i = 0; i <= 50; ++i) {
            const double t = 50 + i * .01;
            const Vector3 center{(i - 50) * .002, 0, 0};
            enrolled.native_observed(t - .02, t, center, true);
            if (i % 5 == 0)
                enrolled.observe(t - .02, t, {add(center, offset), {}}, identity, center, center,
                                 true);
        }
        assert(!enrolled.recent(50.5));
        // Agreement can disappear before the first hand correction is needed.
        auto quiet = enrolled;
        Vector3 creep{};
        for (int i = 1; i <= 550; ++i) {
            const double t = 50.5 + i * .01;
            creep[0] = std::max(0, i - 30) * .00013;
            quiet.native_observed(t - .02, t, creep, false);
        }
        assert(quiet.value(56, creep)[0] > .02 && quiet.value(56, creep)[0] < .038);
        assert(!quiet.recent(56));
        // Settling at a new native position recovers even if no hand returns.
        for (int i = 1; i <= 90; ++i)
            quiet.native_observed(56 + i * .01 - .02, 56 + i * .01, creep, false);
        assert(quiet.value(56.9, creep) == creep);

        auto lost = enrolled;
        const Vector3 wrong{.16, 0, 0};
        for (int i = 1; i <= 90; ++i) {
            const double t = 50.5 + i * .01;
            lost.native_observed(t - .02, t, wrong, true);
            if (i == 5 || i == 10)
                lost.observe(t - .02, t, {offset, {}}, identity, wrong, {}, true);
        }
        assert(!lost.recent(51.4));
        const auto limited = lost.value(51.4, wrong);
        assert(limited[0] >= .059 && limited[0] <= .061);
        const Vector3 far{.8, .3, -.2};
        assert(distance(lost.value(51.4, far), far) <= .100001);

        auto reach = lost;
        for (int i = 1; i <= 40; ++i) {
            const double t = 51.4 + i * .01;
            const Vector3 pt{wrong[0] + std::max(0, i - 10) * .005, 0, 0};
            reach.native_observed(t - .02, t, pt, i > 10);
        }
        assert(reach.value(51.8, far) == far);
        const PalmPoints returned{add(wrong, offset), {}};
        lost.observe(51.5, 51.52, returned, identity, wrong, limited, true);
        lost.observe(51.5, 51.65, returned, identity, wrong, limited, true);
        assert(lost.value(51.66, wrong) == limited);
        lost.observe(51.67, 51.69, returned, identity, wrong, limited, true);
        assert(lost.value(51.69, wrong) == limited);
        assert(lost.value(51.76, wrong)[0] > limited[0] && lost.value(51.85, wrong) == wrong);
        assert(!lost.recent(51.85));
    }
    // Moving a hand away from a parked pad must leave the model on the table.
    {
        HandPosition hands;
        Vector3 base{};
        auto now = 30.;
        for (unsigned i = 0; i < 34; ++i, now += .033) {
            PalmPoints palms;
            const Vector3 visible{i < 12 ? (double(i) - 12) * .005 : 0., 0, 0};
            palms[0] = Vector3{visible[0] - .07 - (i > 12 ? (i - 12) * .004 : 0), 0, 0};
            hands.observe(now, now, palms, identity,
                          i <= 12 ? std::optional<Vector3>(visible) : std::nullopt,
                          hands.value(now, base), i < 12);
            assert(distance(hands.value(now, base), base) < 1e-8);
        }
        assert(!hands.recent(now));
    }
    // Incompatible hands, missing enrollment, and a duplicate timestamp cannot move the pad.
    {
        HandPosition hands;
        PalmPoints palms{Vector3{-.07, 0, 0}, Vector3{.07, 0, 0}};
        for (unsigned i = 0; i < 12; ++i) {
            const Vector3 center{(double(i) - 11) * .005, 0, 0};
            PalmPoints moving{add(*palms[0], center), add(*palms[1], center)};
            hands.observe(60 + i * .033, 60 + i * .033, moving, identity, center, center, true);
        }
        palms[1] = Vector3{.13, 0, 0};
        hands.observe(60.4, 60.4, palms, identity, std::nullopt, {}, true);
        assert(!hands.recent(60.4));
        palms[1] = Vector3{.07, 0, 0};
        hands.observe(60.4, 60.4, palms, identity, std::nullopt, {}, true);
        hands.observe(60.5, 60.5, palms, identity, std::nullopt, {}, true);
        assert(!hands.recent(60.5));
    }
    const Quaternion saved_yaw{std::cos(.25), 0, std::sin(.25), 0};
    waking.rotation.emplace(calibration, "TEST1234", Vector3{});
    waking.orientation = waking.rotation->orientation = identity;
    waking.orientation_time = 40;
    waking.native = NativeObservation{39.98, seen, identity, identity, seen};
    for (int tick = 0; tick < 30; ++tick)
        waking.alignment_samples.emplace_back(39.7 + tick * .01, Vector3{0, 9.80665, 0});
    waking.motion.sample(40, {}, 0);
    assert(waking.motion.observe(39.98, seen, 40) == InertialPosition::Fix::Accepted);
    const TrackingSpace old_space{4, rigid_pose(identity, {})};
    const TrackingSpace new_space{4, rigid_pose(identity, {.25, 0, 0})};
    const TrackingCheckpoint saved{
        "TEST1234", old_space, {30, {.27, 0, -.4}, saved_yaw, identity, {.27, 0, -.4}}};
    assert(!waking.resume(saved, {5, new_space.raw_to_standing}, 40, head));
    assert(!waking.resume(saved, old_space, 40, head));
    assert(!waking.palm_aligned);
    assert(waking.resume(saved, new_space, 40, head));
    assert(waking.palm_aligned && dot(*waking.orientation, saved_yaw) > .999);
    assert(distance(waking.message(40)->position, {.02, 0, -.4}) < 1e-9);
    assert(distance(*waking.motion.value(40.16), seen) < 1e-9);
    std::cout << "native core: boundary validation, wire format and bounded state passed\n";
}
