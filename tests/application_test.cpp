#include "app_replay.h"
#include "controller/reader.h"
#include "platform/linux/error.h"
#include <cassert>
#include <iostream>

namespace {
namespace j = ct::json;
class IntegratedHost final : public app_replay::Host {
  public:
    ct::test::Queue queue;
    ct::QueueContext queues{queue};
    session_replay::Runtime runtime{queues};
    std::shared_ptr<ct::test::FakeState> hid = std::make_shared<ct::test::FakeState>();
    ct::ControllerReader reader{[&] {
                                    return std::make_shared<ct::ControllerDevice>(
                                        std::make_unique<ct::test::FakeTransport>(hid));
                                },
                                [&] { return now; }};
    std::shared_ptr<session_replay::ChildState> helper, filter;
    j::Value helper_events{json_object_new_array()};
    json_object *helper_events_pointer = helper_events.get();
    ct::TrackingSession *session = nullptr;
    IntegratedHost() {
        queue.advancing_status = true;
        queue.connection_time = [&] { return now + 1000; };
    }
    std::unique_ptr<ct::TrackingSessionView> track(const ct::Settings &options) override {
        record("track");
        auto children =
            [&](const std::vector<std::string> &command) -> std::unique_ptr<ct::ChildChannel> {
            assert(command.front() == "/usr/bin/sudo" && command.back() == "--private");
            helper = std::make_shared<session_replay::ChildState>();
            helper->pending.push_back(j::parse(R"({"op":"native_helper_ready","isolated":true})"));
            helper->pending.push_back(j::parse(R"({"op":"input_gate_ready"})"));
            helper->pending.push_back(j::parse(R"({"op":"beam_gate_ready"})"));
            return std::make_unique<session_replay::Channel>(helper, "helper", now,
                                                             helper_events_pointer);
        };
        ct::TrackingCalibration calibration{
            "HEADSET", {1, "TEST1234", {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {}}, {0, .01, -.02}};
        auto result = std::make_unique<ct::TrackingSession>(
            runtime, reader.device(), calibration, options, children,
            [&](const std::string &serial, bool keyboard) -> std::unique_ptr<ct::SteamUiSession> {
                assert(serial == "TEST1234" && !keyboard);
                filter = std::make_shared<session_replay::ChildState>();
                return std::make_unique<session_replay::SteamUi>(filter, now,
                                                                 helper_events_pointer);
            },
            [&] { return now; }, [&] { return now + 1000; },
            [&](double seconds) { now += seconds; });
        session = result.get();
        return result;
    }
};
size_t count(const app_replay::Host &host, const char *kind) {
    size_t result = 0;
    for (size_t i = 0; i < j::array_size(host.trace.get()); ++i)
        result += j::string(j::item(j::item(host.trace.get(), i), 1)) == kind;
    return result;
}
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
void event_boundary(const std::string &event) {
    app_replay::Host host;
    auto current = j::parse("{}");
    host.current = current.get();
    ct::Application app(
        host, {}, std::nullopt, true, [&] { return host.now; }, [&] { return host.now + 1000; });
    app.step(true, true, {});
    assert(count(host, "track") == 1);
    current = j::parse("{\"events\":[" + event + "]}");
    host.current = current.get();
    rejects([&] { app.step(true, true, {}); });
    app.close();
    app.close();
    assert(count(host, "close") == 1);
    assert(!app.running());
}
} // namespace
int main() {
    for (bool enabled : {false, true}) {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Settings options;
        options.steam_shortcuts = enabled;
        ct::Application app(
            host, options, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        ct::ControllerInput input;
        for (uint32_t buttons : {0u, 0x10000u, 0x10200u, 0x10200u, 0x10000u, 0u}) {
            host.now += .01;
            input.received = host.now;
            input.report.buttons = buttons;
            app.step(true, true, {input});
        }
        assert(count(host, "shortcut") == unsigned(enabled));
        for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
            const auto row = j::item(host.trace.get(), i);
            if (j::string(j::item(row, 1)) == "shortcut")
                assert(j::string(j::item(row, 2)) == "screenshot");
        }
        assert(count(host, "keyboard_request") == 0);
        app.close();
    }
    {
        class CameraHost final : public app_replay::Host {
          public:
            std::optional<std::string> error;
            std::optional<std::string> camera_switch_error(bool enabled) override {
                auto result = std::exchange(error, std::nullopt);
                return enabled ? result : std::nullopt;
            }
        } host;
        auto current = j::parse("{}");
        host.current = current.get();
        host.acquisition_complete = false;
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        const auto step = [&](uint32_t buttons) {
            host.now += .05;
            ct::ControllerInput input;
            input.received = host.now;
            input.report.buttons = buttons;
            app.step(true, true, {input});
        };
        const auto attention = [&] {
            for (size_t i = j::array_size(host.trace.get()); i > 0; --i) {
                const auto row = j::item(host.trace.get(), i - 1);
                if (j::string(j::item(row, 1)) != "send")
                    continue;
                const auto value = j::item(row, 2);
                if (j::string(j::field(value, "op")) == "status")
                    return j::boolean(j::field(value, "status_attention"));
            }
            throw std::logic_error("status was not published");
        };
        constexpr uint32_t Camera = 0x10000 | ct::CameraSwitchButton;
        step(0);
        step(Camera);
        assert(count(host, "shortcut") == 0);
        host.acquisition_complete = true;
        step(Camera);
        assert(count(host, "shortcut") == 0);
        step(0);
        step(Camera);
        assert(count(host, "shortcut") == 1);

        host.error = "The passthrough camera did not switch.";
        host.now += .2;
        step(0);
        assert(attention());
        current = j::parse(R"({"events":[{"op":"action","action":"acquire"}]})");
        host.current = current.get();
        step(Camera);
        assert(count(host, "acquire") == 1 && count(host, "shortcut") == 1);

        current = j::parse(R"({"events":[{"op":"action","action":"steam_shortcuts"}]})");
        host.current = current.get();
        host.now += .2;
        step(0);
        assert(!app.options().steam_shortcuts && !attention());
    }
    {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        for (bool enabled : {true, false, true}) {
            current =
                j::parse(std::string("{\"events\":[{\"op\":\"pointer_enabled\",\"enabled\":") +
                         (enabled ? "true" : "false") + "}]}");
            host.current = current.get();
            host.now += .01;
            ct::ControllerInput input;
            input.received = host.now;
            app.step(true, true, {input});
            assert(count(host, "track") == 1 && count(host, "close") == 0);
            bool observed = false;
            for (size_t i = j::array_size(host.trace.get()); i > 0; --i) {
                auto row = j::item(host.trace.get(), i - 1);
                if (j::string(j::item(row, 1)) != "controls")
                    continue;
                assert(j::boolean(j::item(j::item(row, 2), 1)) == enabled);
                observed = true;
                break;
            }
            assert(observed);
        }
        app.close();
    }
    {
        app_replay::Host host;
        host.recovery_mode = true;
        auto current = j::parse(
            R"({"events":[{"op":"action","action":"acquire"}],"fail":{"retry_recovery":{"kind":"runtime","message":"Restart failed"}}})");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        app.step(true, true, {});
        current = j::parse("{}");
        host.current = current.get();
        host.now += 1;
        app.step(true, true, {});
        bool displayed = false;
        for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
            auto row = j::item(host.trace.get(), i);
            if (j::string(j::item(row, 1)) != "send")
                continue;
            auto value = j::item(row, 2);
            if (j::string(j::field(value, "op")) == "status")
                displayed |= j::string(j::field(value, "message")) == "Restart failed";
        }
        assert(displayed);
        app.close();
    }
    {
        app_replay::Host host;
        host.recovery_mode = true;
        auto current = j::parse(
            R"({"events":[{"op":"action","action":"acquire"},{"op":"action","action":"recenter"}]})");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        ct::ControllerInput input;
        input.received = host.now;
        input.report.buttons = 0x10004;
        app.step(true, true, {input});
        assert(count(host, "track") == 0);
        assert(count(host, "shortcut") == 0);
        assert(count(host, "keyboard_request") == 0);
        assert(count(host, "retry_recovery") == 1);
        assert(count(host, "open_uninstaller") == 1);
        app.close();
    }

    {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        ct::ControllerInput input;
        input.received = host.now;
        app.step(true, true, {input});
        std::vector<ct::ControllerInput> batch;
        for (uint32_t buttons : {0x10000u, 0x10004u, 0x10000u, 0u}) {
            input.received += .01;
            input.report.buttons = buttons;
            batch.push_back(input);
        }
        host.now = input.received;
        app.step(true, true, batch);
        assert(count(host, "keyboard_request") == 1 && count(host, "shortcut") == 0);
        app.close();
    }
    {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        app.step(true, true, {});
        app.step(false, false, {});
        assert(count(host, "inspect_permissions") == 0 && count(host, "repair_permissions") == 0);
        current = j::parse(R"({"events":[{"op":"action","action":"inspect_permissions"}]})");
        host.current = current.get();
        app.step(true, false, {});
        assert(count(host, "inspect_permissions") == 1 && count(host, "repair_permissions") == 0);
        current = j::parse(R"({"events":[{"op":"action","action":"repair_permissions"}]})");
        host.current = current.get();
        app.step(true, false, {});
        assert(count(host, "inspect_permissions") == 1 && count(host, "repair_permissions") == 1);
        app.close();
    }
    enum class Wake {
        Disconnect,
        Table,
        DriftOnTable,
        Moved,
        RoomChanged,
        OriginChanged,
        StreamLost,
        LongSleep
    };
    for (auto scenario :
         {Wake::Disconnect, Wake::Table, Wake::DriftOnTable, Wake::Moved, Wake::RoomChanged,
          Wake::OriginChanged, Wake::StreamLost, Wake::LongSleep}) {
        IntegratedHost host;
        host.runtime.space = ct::TrackingSpace{4, ct::rigid_pose({1, 0, 0, 0}, {})};
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        auto reports = host.reader.poll(true);
        app.step(true, host.reader.connected(), reports);
        assert(count(host, "track") == 1 && host.hid->brightness == 100 && host.hid->raw);
        unsigned i = 0;
        double emitter_x = .08;
        auto tick = [&](bool awake, bool optical, double yaw_rate = 0) {
            ++i;
            host.runtime.awake = awake;
            host.now += .01;
            host.hid->time = host.now;
            std::vector<uint8_t> bytes(46);
            bytes[0] = 0x45;
            bytes[37] = 0x40;
            for (unsigned n = 0; n < 4; ++n)
                bytes[30 + n] = uint8_t(i * 10000 >> (8 * n));
            const auto gyro = uint16_t(std::lround(yaw_rate * 16.384 * 180 / std::acos(-1.)));
            bytes[42] = uint8_t(gyro);
            bytes[43] = uint8_t(gyro >> 8);
            host.hid->pending.push_back(bytes);
            ct::wire::PoseBlock pose{};
            pose.controller_id = 16;
            pose.timestamp = host.now + 1000 - .02;
            pose.orientation[1] = -1;
            const auto emitter = ct::transform_point(
                ct::inverse_pose(host.runtime.space->raw_to_standing), {emitter_x, 1.4, -.42});
            pose.position[0] = emitter[0];
            pose.position[1] = -emitter[1];
            pose.position[2] = -emitter[2];
            host.queue.packets[3].resize(sizeof(pose));
            std::memcpy(host.queue.packets[3].data(), &pose, sizeof(pose));
            if (!optical)
                host.queue.packets.erase(3);
            if (optical && i % 10 == 0) {
                auto quality = j::parse(R"({"op":"native_helper_quality","good":true})");
                json_object_object_add(quality.get(), "time",
                                       json_object_new_double(host.now + 1000));
                host.helper->pending.push_back(std::move(quality));
            }
            reports =
                host.reader.poll(awake || (host.reader.connected() && app.keep_controller_open()));
            app.step(awake, host.reader.connected(), reports);
        };
        for (unsigned n = 0; n < 360; ++n)
            tick(true, true);
        assert(host.session->initial.ready);
        if (scenario == Wake::DriftOnTable) {
            // Carry the pad and palm together, then lose the palm while native position runs ahead.
            for (unsigned n = 0; n < 80; ++n) {
                emitter_x += n < 60 ? .0015 : .006;
                if (n % 5 == 0) {
                    auto event = j::parse(
                        R"({"op":"native_helper_palms","samples":[null,{"accepted":true}]})");
                    auto palm = j::item(j::field(event.get(), "samples"), 1);
                    json_object_object_add(palm, "latest_time",
                                           json_object_new_double(host.now + 1000 - .01));
                    json_object_object_add(
                        palm, "pose",
                        j::value(std::array<double, 7>{1, 0, 0, 0, std::min(.17, emitter_x) - .07,
                                                       -1.4, .42}));
                    host.helper->pending.push_back(std::move(event));
                }
                tick(true, true, .14);
            }
            for (int n = 0; n < 48; ++n)
                tick(true, true);
        }
        const auto checkpoint = host.session->checkpoint();
        assert(checkpoint && checkpoint->serial == "TEST1234" && checkpoint->space.universe == 4);
        std::optional<ct::Vector3> rendered;
        for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
            auto row = j::item(host.trace.get(), i);
            if (j::string(j::item(row, 1)) == "send" &&
                j::string(j::field(j::item(row, 2), "op")) == "native_pose")
                rendered = j::numbers<3>(j::field(j::item(row, 2), "position"));
        }
        assert(rendered);
        if (scenario == Wake::DriftOnTable)
            assert(ct::distance(*rendered, checkpoint->pose.position) > .04);
        if (scenario != Wake::Disconnect) {
            const auto original_helper = host.helper;
            // The HMD can lose its pose between the runner's poll and the session's poll.
            host.runtime.awake = false;
            app.step(true, true, {});
            assert(!original_helper->stopped);
            tick(false, false);
            assert(original_helper->stopped && host.filter->stopped && host.hid->raw &&
                   host.hid->brightness == 25);
            if (scenario == Wake::Table) {
                auto change =
                    j::parse(R"({"events":[{"op":"action","action":"brightness_down"}]})");
                host.current = change.get();
                tick(false, false);
                host.current = current.get();
                assert(app.options().brightness == 90 && host.hid->brightness == 25);
            }
            if (scenario == Wake::LongSleep || scenario == Wake::StreamLost) {
                host.now += scenario == Wake::LongSleep ? 31 : .25;
                app.step(false, true, {});
                assert(!app.keep_controller_open() && !host.hid->raw);
                continue;
            }
            for (unsigned n = 0; n < 50; ++n)
                tick(false, false, scenario == Wake::Moved ? 1 : 0);
            const ct::Quaternion relocalized{std::cos(.15), 0, std::sin(.15), 0};
            host.runtime.space = ct::TrackingSpace{99, ct::rigid_pose(relocalized, {.25, 0, 0})};
            host.runtime.head = ct::rigid_pose({0, 0, 1, 0}, {0, 1.6, 0});
            for (unsigned n = 0; n < 100; ++n)
                tick(true, false);
            assert(host.helper == original_helper);
            if (scenario == Wake::RoomChanged) {
                for (unsigned n = 0; n < 210; ++n)
                    tick(true, false);
                assert(count(host, "track") == 2 && !host.session->initial.ready);
                continue;
            }
            host.runtime.space->universe = 4;
            const auto before = j::array_size(host.trace.get());
            const auto first_command = host.queue.writes.size();
            current = j::parse(R"({"events":[{"op":"pointer_enabled","enabled":true}]})");
            host.current = current.get();
            for (unsigned n = 0; n < 240; ++n)
                tick(true, false);
            assert(count(host, "track") == 1 && host.helper != original_helper &&
                   !host.session->initial.ready && !host.session->input_state().native_active);
            if (scenario == Wake::Table) {
                assert(host.hid->brightness == 90);
                for (unsigned n = 0; n < 100; ++n) {
                    auto quality = j::parse(R"({"op":"native_helper_quality","good":true})");
                    json_object_object_add(quality.get(), "time",
                                           json_object_new_double(host.now + 1000));
                    host.helper->pending.push_back(std::move(quality));
                    tick(true, false);
                }
                assert(host.session->initial.ready && !host.session->acquired());
            }
            unsigned input_packets = 0;
            for (size_t n = first_command; n < host.queue.writes.size(); ++n) {
                const auto &command = host.queue.writes[n];
                if (command.kind == 3) {
                    ct::wire::AppInput input{};
                    std::memcpy(&input, command.data, sizeof(input));
                    assert(input.flags == 0 && input.pose_time == 0);
                    ++input_packets;
                }
            }
            assert(input_packets > 0);
            bool parked = false;
            for (size_t row = before; row < j::array_size(host.trace.get()); ++row) {
                auto entry = j::item(host.trace.get(), row);
                if (j::string(j::item(entry, 1)) != "send")
                    continue;
                auto value = j::item(entry, 2);
                if (j::string(j::field(value, "op")) == "native_pose") {
                    auto point = j::numbers<3>(j::field(value, "position"));
                    const auto standing =
                        ct::transform_point(host.runtime.space->raw_to_standing, point);
                    assert(ct::distance(standing, *rendered) < .001);
                    assert(j::number(j::field(value, "time")) < host.now + 1000 - 1);
                    parked = true;
                }
            }
            assert(parked == (scenario != Wake::Moved));
            if (scenario == Wake::OriginChanged) {
                host.runtime.space->universe = 8;
                auto event = j::parse(
                    R"({"events":[{"op":"origin_changed","transform":[1,0,0,0.3,0,1,0,0,0,0,1,0]}]})");
                host.current = event.get();
                tick(true, false);
                host.current = current.get();
            }
            const unsigned optical_ticks = scenario == Wake::OriginChanged ? 150 : 95;
            for (unsigned n = 0; n < optical_ticks; ++n)
                tick(true, true);
            const auto restored = host.session->checkpoint();
            assert(restored && host.session->initial.ready);
            const auto standing_heading = ct::multiply(relocalized, restored->pose.orientation);
            const double angle =
                2 * std::acos(std::min(
                        1., std::abs(ct::dot(checkpoint->pose.orientation, standing_heading))));
            if (scenario == Wake::Moved)
                assert(angle > .45 && angle < .55);
            else if (scenario == Wake::OriginChanged)
                assert(angle > 2.5);
            else
                assert(angle < .001);
            continue;
        }
        host.hid->receive_error = ENODEV;
        reports = host.reader.poll(true);
        assert(!host.reader.connected() && !host.hid->closed);
        app.step(true, false, reports);
        assert(host.hid->closed == 1 && host.helper->stopped && host.filter->stopped);
        assert(host.queue.opened.size() == host.queue.destroyed.size() + 1);
        assert(std::count(host.queue.opened.begin(), host.queue.opened.end(), 1) == 1);
        host.hid->receive_error = 0;
        host.now += 1;
        reports = host.reader.poll(true);
        app.step(true, host.reader.connected(), reports);
        assert(count(host, "track") == 2);
        host.runtime.awake = false;
        reports = host.reader.poll(false);
        app.step(false, host.reader.connected(), reports);
        assert(host.hid->closed == 2 && host.hid->brightness == 25 && !host.hid->raw);
        assert(host.queue.opened.size() == host.queue.destroyed.size() + 1);
        app.close();
        host.queues.close();
        assert(host.queue.opened.size() == host.queue.destroyed.size());
    }
    for (bool tracking_unavailable : {false, true}) {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {},
            tracking_unavailable ? std::optional<std::string>("Tracking compatibility unavailable")
                                 : std::nullopt,
            true, [&] { return host.now; }, [&] { return host.now + 1000; });
        ct::ControllerInput input;
        input.received = host.now;
        app.step(true, true, {input});
        host.now += .01;
        input.received = host.now;
        input.report.buttons = 0x10004;
        app.step(true, true, {input});
        assert(count(host, "keyboard_request") == 1);
        assert(count(host, "track") == (tracking_unavailable ? 0 : 1));
        if (tracking_unavailable) {
            bool displayed = false;
            for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
                auto row = j::item(host.trace.get(), i);
                if (j::string(j::item(row, 1)) != "send")
                    continue;
                auto value = j::item(row, 2);
                if (j::string(j::field(value, "op")) == "status")
                    displayed |= j::string(j::field(value, "message")) ==
                                 "Tracking compatibility unavailable";
            }
            assert(displayed);
        }
        app.close();
    }
    {
        app_replay::Host host;
        auto current = j::parse(
            R"({"fail":{"progress":{"kind":"runtime","message":"animation unavailable"}}})");
        host.current = current.get();
        ct::Settings options;
        options.steam_shortcuts = true;
        ct::Application app(
            host, options, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        ct::ControllerInput input;
        input.received = host.now;
        app.step(true, true, {input});
        input.report.buttons = 0x10000;
        for (int i = 0; i < 13; ++i) {
            host.now += .1;
            input.received = host.now;
            app.step(true, true, {input});
        }
        input.report.buttons = 0;
        host.now += .1;
        input.received = host.now;
        app.step(true, true, {input});
        assert(count(host, "progress") == 1);
        assert(count(host, "shortcut") == 1);
        assert(count(host, "track") == 1);
        bool problem_displayed = false;
        for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
            auto row = j::item(host.trace.get(), i);
            if (j::string(j::item(row, 1)) != "send")
                continue;
            auto value = j::item(row, 2);
            if (j::string(j::field(value, "op")) == "status") {
                const auto text = j::string(j::field(value, "message"));
                problem_displayed |=
                    text.find("Shortcut animation unavailable") != std::string::npos;
            }
        }
        assert(problem_displayed);
        assert(count(host, "controls") > 0);
        app.close();
    }
    {
        app_replay::Host host;
        auto current =
            j::parse(R"({"fail":{"track":{"kind":"runtime","message":"camera offline"}}})");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        app.step(true, true, {});
        bool problem_displayed = false;
        for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
            auto row = j::item(host.trace.get(), i);
            if (j::string(j::item(row, 1)) != "send")
                continue;
            auto value = j::item(row, 2);
            if (j::string(j::field(value, "op")) == "status") {
                const auto text = j::string(j::field(value, "message"));
                problem_displayed |= text.find("camera offline") != std::string::npos;
            }
        }
        assert(problem_displayed);
        app.close();
    }
    assert(ct::retryable(ct::ConnectionError("disconnected")));
    assert(ct::retryable(ct::TimeoutError("late")));
    assert(ct::retryable(std::system_error(EIO, std::generic_category())));
    for (auto message : {"Headset asleep", "Native tracking service restarted. Reconnect.",
                         "Controller IMU stream stopped", "Native tracking service restarted"})
        assert(ct::retryable(std::runtime_error(message)));
    assert(!ct::retryable(std::runtime_error("invalid native package")));
    assert(!ct::retryable(std::invalid_argument("malformed input")));
    for (
        const auto event :
        {R"({"op":"pointer_enabled","enabled":1})",
         R"({"op":"pointer_enabled","enabled":true,"extra":true})", R"({"op":"pointer_enabled"})",
         R"({"op":"keyboard_state","time":1100,"visible":true,"gaze_valid":false,"gaze_on_keyboard":true})",
         R"({"op":"keyboard_state","time":1100.021,"visible":true,"gaze_valid":true,"gaze_on_keyboard":true})",
         R"({"op":"keyboard_state","time":1100,"visible":1,"gaze_valid":true,"gaze_on_keyboard":true})",
         R"({"op":"keyboard_state","time":1100,"visible":true,"gaze_valid":true})",
         R"({"op":"origin_changed","transform":[1,0]})"})
        event_boundary(event);
    {
        struct WakeHost final : app_replay::Host {
            bool controllers_ready = false;
            bool stock_tracking_ready() override {
                return controllers_ready;
            }
        } host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        app.step(true, true, {});
        app.step(false, false, {});
        app.step(true, true, {});
        host.now += 1;
        app.step(true, true, {});
        assert(count(host, "close") == 1 && count(host, "track") == 1);
        app.step(false, false, {});
        app.step(true, true, {});
        host.controllers_ready = true;
        host.now += .5;
        app.step(true, true, {});
        assert(count(host, "track") == 1);
        host.now += .3;
        app.step(true, true, {});
        assert(count(host, "track") == 2);

        app.step(false, false, {});
        app.step(true, true, {});
        host.controllers_ready = false;
        host.now += 3.01;
        app.step(true, true, {});
        assert(count(host, "track") == 3);
    }
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        app.step(true, false, {});
        assert(count(host, "track") == 0 && app.cadence() == .02);
        app.step(true, true, {});
        assert(count(host, "track") == 1 && app.cadence() == .004);
        app.step(false, true, {});
        assert(count(host, "close") == 1 && app.cadence() == .02);
        app.step(true, true, {});
        assert(count(host, "track") == 1);
        host.now += .76;
        app.step(true, true, {});
        assert(count(host, "track") == 2);
        app.show_settings();
        app.step(true, true, {});
        app.stop();
        const auto records = j::array_size(host.trace.get());
        app.step(true, true, {});
        assert(j::array_size(host.trace.get()) == records);
        app.close();
        app.close();
        assert(count(host, "close") == 2);
    }
    {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, std::nullopt, true, [&] { return host.now; },
            [&] { return host.now + 1000; });
        app.step(true, true, {});
        rejects([&] { app.step(true, true, std::vector<ct::ControllerInput>(257)); });
        current = j::parse(
            R"({"fail":{"step":{"kind":"connection","message":"input lost"},"close":{"kind":"runtime","message":"restore failed"}}})");
        host.current = current.get();
        app.step(true, true, {});
        assert(count(host, "close") == 1 && count(host, "track") == 1);
        current = j::parse("{}");
        host.current = current.get();
        host.now += 4.99;
        app.step(true, true, {});
        assert(count(host, "track") == 1);
        host.now += .02;
        app.step(true, true, {});
        assert(count(host, "track") == 2);
        app.close();
        assert(count(host, "close") == 2);
    }
    std::cout << "application: event boundaries, failure cleanup, retries and 100 lifecycle cycles "
                 "passed\n";
}
