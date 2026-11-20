#include "ui/presentation.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

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
auto parse_command(const std::string &text) {
    return ct::ui::command(ct::json::parse(text).get());
}
int main() {
    using namespace ct::ui;
    assert(!parse_command(std::string(R"({"op":"unknown"})")));
    assert(std::get<Operation>(*parse_command(std::string(R"({"op":"input_off"})"))) ==
           Operation::InputOff);
    auto feedback = parse_command(std::string(
        R"({"op":"tracking_feedback","active":true,"state":"realign","message":"View + Menu","time":42,"progress":0.5})"));
    assert(std::get<Feedback>(*feedback).progress == .5);
    auto pose = parse_command(std::string(
        R"({"op":"native_pose","time":42,"position_time":42,"orientation_time":42,"position":[0,0,-0.4],"orientation":[1,0,0,0]})"));
    assert(std::get<ct::BodyPose>(*pose).optical_time == 42);
    for (
        const auto text :
        {R"({"op":"controls","time":1,"buttons":4294967296})",
         R"({"op":"keyboard_typing","active":null})",
         R"({"op":"tracking_feedback","active":true,"state":"other","message":"","time":1,"progress":0})",
         R"({"op":"tracking_feedback","active":false,"extra":1})",
         R"({"op":"native_pose","time":42,"position_time":42,"orientation_time":42,"position":[0,0,-0.4],"orientation":[0,0,0,0]})"})
        rejects([&] { parse_command(std::string(text)); });
    auto gaze = event_json(KeyboardState{42, true, true, false});
    assert(ct::json::string(ct::json::field(gaze.get(), "op")) == "keyboard_state");
    assert(ct::json::boolean(ct::json::field(gaze.get(), "visible")));
    assert(!ct::json::boolean(ct::json::field(gaze.get(), "gaze_on_keyboard")));
    for (bool events : {false, true}) {
        Presentation channel;
        for (unsigned i = 0; i < 128; ++i) {
            if (events)
                channel.emit(Action{std::to_string(i)});
            else
                channel.send(Controls{double(i + 1), i, {}, {}, {}, {}, {}});
        }
        rejects([&] {
            if (events)
                channel.emit(Quit{});
            else
                channel.send(Operation::Quit);
        });
        assert(!channel.running());
        if (events) {
            auto items = channel.events();
            assert(items.size() == 128);
            for (unsigned i = 0; i < 128; ++i)
                assert(std::get<Action>(items[i]).name == std::to_string(i));
        } else {
            auto items = channel.commands();
            assert(items.size() == 128);
            for (unsigned i = 0; i < 128; ++i)
                assert(std::get<Controls>(items[i]).buttons == i);
        }
    }
    Presentation channel;
    constexpr unsigned count = 10000;
    std::atomic<unsigned> received{0};
    std::thread consumer([&] {
        while (channel.running()) {
            for (const auto &message : channel.commands()) {
                const auto &input = std::get<Controls>(message);
                const auto index = received++;
                assert(input.time == index + 1 && input.buttons == (index & 1));
                channel.emit(Action{std::to_string(index + 1)});
            }
            std::this_thread::yield();
        }
    });
    unsigned acknowledged = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (unsigned i = 0; i < count; ++i) {
        channel.send(Controls{double(i + 1), i & 1, {}, {}, {}, {}, {}});
        if (i % 64 == 63 || i + 1 == count) {
            while (acknowledged <= i) {
                assert(std::chrono::steady_clock::now() < deadline);
                for (const auto &event : channel.events())
                    assert(std::get<Action>(event).name == std::to_string(++acknowledged));
                std::this_thread::yield();
            }
        }
    }
    channel.close();
    consumer.join();
    assert(received == count && acknowledged == count);
    channel.send(Operation::InputOff);
    assert(channel.commands().empty());
    std::cout << "UI presentation: typed validation, bounded failure, 10000 ordered input edges "
                 "and event replies passed\n";
}
