#include "session_replay.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

namespace {
struct Fixture {
    ct::test::Queue queue;
    ct::QueueContext queues{queue};
    session_replay::Runtime runtime{queues};
    std::shared_ptr<ct::test::FakeState> hid = std::make_shared<ct::test::FakeState>();
    std::shared_ptr<ct::ControllerDevice> controller =
        std::make_shared<ct::ControllerDevice>(std::make_unique<ct::test::FakeTransport>(hid));
    double now = 100;
    ct::TrackingCalibration calibration{
        "HEADSET", {1, "TEST1234", {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {}}, {0, .01, -.02}};
    std::shared_ptr<session_replay::ChildState> helper, filter;
    ct::json::Value events{json_object_new_array()};
    json_object *events_pointer = events.get();
    std::string fail_child;
    Fixture() {
        queue.advancing_status = true;
        queue.connection_time = [&] { return now + 1000; };
    }
    std::unique_ptr<ct::TrackingSession>
    create(std::function<void(const ct::TrackingCalibration &)> save_calibration = {}) {
        helper = std::make_shared<session_replay::ChildState>();
        filter = std::make_shared<session_replay::ChildState>();
        helper->pending.push_back(
            ct::json::parse(R"({"op":"native_helper_ready","isolated":true})"));
        helper->pending.push_back(ct::json::parse(R"({"op":"input_gate_ready"})"));
        auto factory =
            [&](const std::vector<std::string> &command) -> std::unique_ptr<ct::ChildChannel> {
            assert(command[0] == "/usr/bin/sudo" && command.back() == "--private");
            if (fail_child == "helper")
                throw std::runtime_error("injected helper startup failure");
            return std::make_unique<session_replay::Channel>(helper, "helper", now, events_pointer);
        };
        return std::make_unique<ct::TrackingSession>(
            runtime, controller, calibration, ct::Settings{}, factory,
            [&](const std::string &serial, bool keyboard) -> std::unique_ptr<ct::SteamUiSession> {
                assert(serial == "TEST1234" && !keyboard);
                if (fail_child == "filter")
                    throw std::runtime_error("injected filter startup failure");
                return std::make_unique<session_replay::SteamUi>(filter, now, events_pointer);
            },
            [&] { return now; }, [&] { return now + 1000; },
            [&](double seconds) { now += seconds; }, std::move(save_calibration));
    }
    ct::ControllerInput report(unsigned tick) const {
        ct::ControllerInput input;
        input.received = now;
        input.report.sensor_tick = tick * 10000;
        input.report.acceleration = {0, 9.80665, 0};
        return input;
    }
};
template <class Function> void rejects(Function function, const std::string &expected) {
    bool failed = false;
    try {
        function();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(expected) != std::string::npos;
        if (!failed)
            std::cerr << "Expected " << expected << ", received " << error.what() << '\n';
    }
    assert(failed);
}
} // namespace
int main() {
    {
        Fixture f;
        auto session = f.create();
        const auto started = f.now;
        for (unsigned tick = 0; tick < 230; ++tick) {
            f.now = started + tick * .01;
            session->step({f.report(tick)});
        }
        const auto commands = f.hid->commands.size();
        f.now += .12;
        session->step({});
        assert(f.hid->commands.size() == commands + 2);
        assert(f.hid->commands[commands] == ct::setting_report(0x30, 0x18));
        const auto feedback = session->presentation();
        assert(feedback && ct::json::string(ct::json::field(feedback.get(), "state")) != "imu");
        session->close();
    }
    {
        Fixture f;
        auto session = f.create();
        f.controller.reset();
        assert(!f.hid->closed && f.hid->brightness == 100);
        session->close();
        assert(!f.hid->closed && f.hid->brightness == 25 && !f.hid->raw);
        session.reset();
        assert(f.hid->closed == 1);
    }
    {
        Fixture f;
        f.calibration.headset_serial = "OTHER";
        rejects([&] { f.create(); }, "another headset");
        assert(f.hid->commands.empty() && !f.hid->closed);
    }
    {
        Fixture f;
        f.runtime.awake = false;
        rejects([&] { f.create(); }, "Put the Frame on");
        assert(f.hid->commands.empty());
    }
    {
        Fixture f;
        f.queue.connect_errors["/xrservice/controller/data"] = 2;
        rejects([&] { f.create(); }, "startup is not ready");
        assert(f.hid->commands.empty());
    }
    {
        Fixture f;
        f.calibration.body.controller_serial = "OTHER";
        rejects([&] { f.create(); }, "different controller");
        assert(!f.hid->raw && f.hid->brightness == 25 && !f.hid->closed);
    }
    {
        Fixture f;
        f.fail_child = "helper";
        rejects([&] { f.create(); }, "startup failure");
        assert(!f.hid->raw && f.hid->brightness == 25 && !f.hid->closed);
    }
    {
        Fixture f;
        f.fail_child = "filter";
        auto session = f.create();
        assert(session->input_state().error == "injected filter startup failure");
        session->step({f.report(0)});
        session->close();
        assert(f.helper->stopped && !f.hid->raw && f.hid->brightness == 25);
    }
    {
        Fixture f;
        auto session = f.create();
        f.helper->pending[0] = ct::json::parse(R"({"op":"native_helper_ready"})");
        rejects([&] { session->step({f.report(0)}); }, "did not confirm isolated");
        session->close();
        assert(f.helper->stopped && f.filter->stopped && !f.hid->raw);
    }
    {
        Fixture f;
        auto session = f.create();
        session->step({f.report(0)});
        f.hid->restoration_error = EPERM;
        f.filter->close_error = "injected filter cleanup failure";
        f.helper->restore = false;
        std::string error;
        try {
            session->close();
        } catch (const std::exception &failure) {
            error = failure.what();
        }
        for (const auto *text : {"filter cleanup failure", "controller cleanup",
                                 "compatibility restoration was not confirmed"})
            assert(error.find(text) != std::string::npos);
        assert(f.filter->stopped && f.helper->stopped && !f.hid->raw && !f.hid->closed);
        session->close();
        f.hid->restoration_error = 0;
        f.controller->restore();
        assert(f.hid->brightness == 25);
    }
    {
        Fixture f;
        for (unsigned cycle = 0; cycle < 100; ++cycle) {
            auto session = f.create();
            const double started = f.now;
            for (unsigned tick = 0; tick < 215; ++tick) {
                f.now = started + tick * .01;
                session->controls(f.report(tick), false);
                session->step({f.report(tick)});
            }
            assert(session->status ==
                   "Native tracking is finding the gamepad. Hold it in front of the headset.");
            session->close();
            assert(f.helper->stopped && f.filter->stopped && !f.hid->raw &&
                   f.hid->brightness == 25);
            assert(!f.hid->closed && f.controller->read(.01));
            rejects([&] { session->step({}); }, "session is closed");
            rejects([&] { session->acquire(); }, "session is closed");
            rejects([&] { session->update_options({}); }, "session is closed");
            f.now += .01;
            f.events.reset(json_object_new_array());
            f.events_pointer = f.events.get();
        }
        assert(std::count(f.queue.opened.begin(), f.queue.opened.end(), 1) == 1);
        assert(std::count(f.queue.destroyed.begin(), f.queue.destroyed.end(), 2) == 100);
        assert(std::count(f.queue.destroyed.begin(), f.queue.destroyed.end(), 3) == 100);
    }
    {
        Fixture f;
        f.calibration.gyro_calibrated = false;
        std::optional<ct::TrackingCalibration> saved;
        unsigned saves = 0;
        auto session = f.create([&](const ct::TrackingCalibration &value) {
            saved = value;
            ++saves;
        });
        const auto started = f.now;
        const ct::Vector3 bias{.0001, -.0002, .0003};
        for (unsigned tick = 0; tick < 225; ++tick) {
            f.now = started + tick * .01;
            auto input = f.report(tick);
            input.report.angular_velocity = bias;
            session->step({input});
        }
        assert(saved && saved->gyro_calibrated && saves == 1);
        assert(saved->body.controller_serial == "TEST1234");
        for (size_t index = 0; index < bias.size(); ++index)
            assert(std::abs(saved->body.gyro_bias_report_rad_s[index] - bias[index]) < 1e-12);
        session->close();
    }
    std::cout << "tracking session: fresh gyro calibration, startup failure, ownership, "
                 "restoration and 100 full queue "
                 "cycles passed\n";
}
