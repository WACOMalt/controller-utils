#include "steamvr/steam_ui.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <filesystem>

namespace {
using namespace ct;
struct State {
    std::mutex mutex;
    std::vector<SteamUiRequest> requests;
    std::atomic<unsigned> connects{0}, live{0};
    std::atomic<bool> fail{false}, cleanup_failure{false}, wrong_identity{false}, retain{false};
    std::atomic<bool> block{false}, entered{false}, proceed{false};
    std::atomic<bool> invalid_flag{false};
    std::condition_variable changed;
    size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return requests.size();
    }
    SteamUiRequest last() {
        std::lock_guard<std::mutex> lock(mutex);
        assert(!requests.empty());
        return requests.back();
    }
};
class Connection final : public SteamUiConnection {
    std::shared_ptr<State> state_;

  public:
    explicit Connection(std::shared_ptr<State> state) : state_(std::move(state)) {
        ++state_->live;
    }
    ~Connection() override {
        --state_->live;
    }
    json::Value renew(const SteamUiRequest &request) override {
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->requests.push_back(request);
            if (request.enabled && state_->block) {
                state_->entered = true;
                state_->changed.wait(lock, [&] { return state_->proceed.load(); });
            }
        }
        if (state_->fail || (state_->cleanup_failure && !request.enabled && !request.typing))
            throw std::runtime_error("fixture Steam debugger disconnected");
        const bool active = request.enabled || state_->retain;
        json::Value result(json_object_new_object());
        json_object_object_add(result.get(), "active",
                               state_->invalid_flag ? json_object_new_int(active)
                                                    : json_object_new_boolean(active));
        json_object_object_add(result.get(), "index", json_object_new_int(active ? 3 : -1));
        json_object_object_add(
            result.get(), "serial",
            json_object_new_string(state_->wrong_identity ? "OTHER" : request.serial.c_str()));
        json_object_object_add(result.get(), "keyboardHealth",
                               json_object_new_string(request.typing ? "ready" : "off"));
        json_object_object_add(result.get(), "keyboardWatch",
                               json_object_new_boolean(request.typing));
        return result;
    }
};
SteamUiConnect connect(const std::shared_ptr<State> &state) {
    return [state] {
        ++state->connects;
        return std::make_unique<Connection>(state);
    };
}
template <class Function> void wait(Function condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
template <class Function> void rejects(Function function, const std::string &expected) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &error) {
        rejected = std::string(error.what()).find(expected) != std::string::npos;
    }
    assert(rejected);
}
FilterState state(SteamUiSession &client, bool active, const std::string &health = "off") {
    std::optional<FilterState> found;
    wait([&] {
        for (const auto &event : client.poll())
            if (const auto value = std::get_if<FilterState>(&event);
                value && value->active == active && value->keyboard_health == health)
                found = *value;
        return bool(found);
    });
    return *found;
}
void error(SteamUiSession &client, const std::string &expected) {
    bool found = false;
    wait([&] {
        for (const auto &event : client.poll())
            if (const auto value = std::get_if<FilterError>(&event))
                found |= value->message.find(expected) != std::string::npos;
        return found;
    });
}
size_t descriptors() {
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
} // namespace
int main() {
    using namespace ct;
    std::atomic<double> now{100};
    const auto clock = [&] { return now.load(); };
    const auto before = descriptors();
    std::string previous_owner;
    {
        auto log = std::make_shared<State>();
        SteamUiClient client("TEST1234", false, connect(log), clock);
        FilterRequest request;
        request.enabled = request.system_only = true;
        client.send(request);
        state(client, true);
        assert(log->last().system_only && log->last().tap == 0);
        request.tap = 1;
        client.send(request);
        wait([&] { return log->last().tap == 1; });
        client.close();
        assert(log->last().system_only && !log->last().enabled && log->last().tap == 1);
    }
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        auto log = std::make_shared<State>();
        now = 100;
        SteamUiClient client("TEST1234", false, connect(log), clock);
        assert(state(client, false).serial == "TEST1234" && !log->connects);
        client.send(FilterRequest{true});
        assert(state(client, true).index == 3 && log->connects == 1);
        const auto owner = log->last().owner;
        assert(!owner.empty() && owner != previous_owner);
        previous_owner = owner;
        client.send(FilterRequest{false, true, true, 3, true});
        assert(state(client, false, "ready").time == now);
        assert(log->last().typing && log->last().touches == 3 && log->last().held);
        client.send(FilterRequest{false, true, false, 0, false});
        state(client, false);
        client.close();
        assert(!client.running() && !log->live && !log->last().enabled && !log->last().typing);
        assert(log->last().owner == owner && descriptors() == before);
    }
    {
        auto log = std::make_shared<State>();
        SteamUiClient client("TEST1234", false, connect(log), clock);
        client.send(FilterRequest{true});
        state(client, true);
        now = 102.01;
        state(client, false);
        assert(!log->last().enabled);
        client.close();
    }
    {
        now = 100;
        auto log = std::make_shared<State>();
        log->fail = true;
        SteamUiClient client("TEST1234", false, connect(log), clock);
        client.send(FilterRequest{true});
        error(client, "disconnected");
        log->fail = false;
        now = 100.51;
        state(client, true);
        assert(log->connects >= 2);
        client.close();
        assert(!log->last().enabled && !log->live);
    }
    {
        now = 100;
        auto log = std::make_shared<State>();
        log->wrong_identity = true;
        SteamUiClient client("TEST1234", false, connect(log), clock);
        client.send(FilterRequest{true});
        error(client, "invalid identity");
        log->wrong_identity = false;
        now = 100.51;
        state(client, true);
        client.close();
    }
    for (bool retained : {false, true}) {
        auto log = std::make_shared<State>();
        SteamUiClient client("TEST1234", false, connect(log), clock);
        client.send(FilterRequest{true});
        state(client, true);
        if (retained)
            log->retain = true;
        else
            log->cleanup_failure = true;
        rejects([&] { client.close(); }, retained ? "release was not confirmed" : "cleanup");
        client.close();
        assert(!client.running() && !log->live);
    }
    {
        now = 100;
        auto log = std::make_shared<State>();
        log->invalid_flag = true;
        SteamUiClient client("TEST1234", false, connect(log), clock);
        client.send(FilterRequest{true});
        error(client, "boolean");
        log->invalid_flag = false;
        now = 100.51;
        state(client, true);
        client.close();
    }
    {
        auto log = std::make_shared<State>();
        SteamUiClient client("keyboard-shortcut", true, connect(log), clock);
        wait([&] { return !client.running(); });
        state(client, false);
        client.close();
        assert(log->count() == 1 && log->last().keyboard && log->last().action_only &&
               !log->last().enabled && !log->live);
    }
    {
        auto log = std::make_shared<State>();
        log->fail = true;
        SteamUiClient client("keyboard-shortcut", true, connect(log), clock);
        wait([&] { return !client.running(); });
        error(client, "disconnected");
        client.close();
        assert(log->count() == 1 && !log->live);
    }
    {
        auto log = std::make_shared<State>();
        SteamUiClient client("TEST1234", false, connect(log), clock);
        rejects([&] { client.send(FilterRequest{true, true, true}); }, "Invalid keyboard");
        rejects([&] { client.send(FilterRequest{false, true, true, 4}); }, "Invalid keyboard");
        client.close();
        rejects([&] { client.send({}); }, "stopped");
        assert(!log->connects);
    }
    {
        auto log = std::make_shared<State>();
        log->block = true;
        SteamUiClient client("TEST1234", false, connect(log), clock);
        client.send(FilterRequest{true});
        wait([&] { return log->entered.load(); });
        std::atomic<bool> closed{false};
        std::thread closing([&] {
            client.close();
            closed = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(!closed);
        log->proceed = true;
        log->changed.notify_one();
        closing.join();
        assert(closed && !log->last().enabled && !log->live && !client.running());
    }
    {
        auto log = std::make_shared<State>();
        SteamUiClient client("TEST1234", false, connect(log), clock);
        for (unsigned i = 0; i < 140 && client.running(); ++i) {
            const auto count = log->count();
            try {
                client.send(FilterRequest{i % 2 == 0, true, i % 2 != 0});
            } catch (const std::runtime_error &) {
                break;
            }
            wait([&] { return log->count() > count || !client.running(); });
        }
        wait([&] { return !client.running(); });
        auto events = client.poll();
        assert(events.size() <= 129);
        bool limit = false;
        for (const auto &event : events)
            if (const auto value = std::get_if<FilterError>(&event))
                limit |= value->message.find("event limit") != std::string::npos;
        assert(limit && !log->last().enabled && !log->last().typing && !log->live);
        rejects([&] { client.close(); }, "event limit");
    }
    rejects(
        [&] { SteamUiClient client("invalid/serial", false, connect(std::make_shared<State>())); },
        "valid controller serial");
    assert(descriptors() == before);
    std::cout << "Steam UI: 100 owned thread/lease lifecycles, typing, heartbeat expiry, "
                 "reconnect, identity refusal, keyboard requests and cleanup failures passed\n";
}
