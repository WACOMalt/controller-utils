#include "steam_ui.h"
#include "platform/linux/resource.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <utility>
#include <unistd.h>

namespace ct {
namespace {
FilterState confirmation(json_object *result, const std::string &serial, double now) {
    const auto active = json::boolean(json::field(result, "active"));
    const auto index = json::integer(json::field(result, "index"));
    if (json::string(json::field(result, "serial")) != serial ||
        (active && (index < 0 || index >= 16)) || (!active && index != -1))
        throw std::runtime_error("Steam input filter returned an invalid identity");
    const auto health = json::find(result, "keyboardHealth");
    return {active, int(index), serial, now, health ? json::string(health) : "off"};
}
bool keyboard_watch(json_object *result) {
    const auto watch = json::find(result, "keyboardWatch");
    return watch && json::boolean(watch);
}
std::string bounded(const std::string &text) {
    return text.substr(0, 300);
}
} // namespace
SteamUiClient::SteamUiClient(std::string serial, bool keyboard_only, SteamUiConnect connect,
                             std::function<double()> monotonic)
    : serial_(std::move(serial)), keyboard_only_(keyboard_only),
      connect_(connect ? std::move(connect) : connect_steam_ui),
      clock_(monotonic ? std::move(monotonic) : monotonic_time) {
    if (!steamui::serial_valid(serial_))
        throw std::invalid_argument("Expected one valid controller serial");
    static std::atomic<uint64_t> generation{0};
    owner_ = std::to_string(getpid()) + "-" + std::to_string(raw_time_ns()) + "-" +
             std::to_string(++generation);
    worker_ = std::thread([this] { run(); });
}
SteamUiClient::~SteamUiClient() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
}
bool SteamUiClient::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}
void SteamUiClient::send(const FilterRequest &request) {
    if (keyboard_only_)
        throw std::logic_error("Keyboard request cannot acquire controller input");
    auto next = request;
    if (!next.details) {
        next.typing = false;
        next.touches = 0;
        next.held = false;
    }
    if ((next.enabled && next.typing) || next.touches > 3 ||
        (next.system_only && (next.details || next.typing)))
        throw std::invalid_argument("Invalid keyboard input handoff");
    const auto time = clock_();
    if (!std::isfinite(time) || time < 0)
        throw std::invalid_argument("Invalid Steam UI heartbeat time");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !running_)
            throw std::runtime_error("Steam input filter stopped");
        immediate_ |= request_.enabled != next.enabled || request_.typing != next.typing ||
                      request_.tap != next.tap;
        request_ = next;
        heartbeat_ = time;
    }
    changed_.notify_one();
}
std::vector<FilterEvent> SteamUiClient::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FilterEvent> result;
    result.swap(events_);
    if (terminal_error_) {
        result.push_back(FilterError{*terminal_error_});
        terminal_error_.reset();
    }
    return result;
}
void SteamUiClient::publish(FilterEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() == 128)
        throw std::runtime_error("Steam input filter event limit exceeded");
    events_.push_back(std::move(event));
}
void SteamUiClient::run() {
    std::unique_ptr<SteamUiConnection> connection;
    bool cleanup_needed = false;
    double next = 0;
    std::string last_error;
    try {
        if (keyboard_only_) {
            connection = connect_();
            if (!connection)
                throw std::runtime_error("Steam UI connection is unavailable");
            auto result = connection->renew({serial_, owner_, false, true, true});
            publish(confirmation(result.get(), serial_, clock_()));
        } else {
            while (true) {
                FilterRequest request;
                double heartbeat;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (clock_() < next && !immediate_ && !stopping_)
                        changed_.wait_for(lock, std::chrono::milliseconds(25),
                                          [&] { return stopping_ || immediate_; });
                    if (stopping_)
                        break;
                    if (std::exchange(immediate_, false))
                        next = 0;
                    request = request_;
                    heartbeat = heartbeat_;
                }
                const auto now = clock_();
                if (now < next)
                    continue;
                const bool enabled = request.enabled && now - heartbeat < 2;
                const bool typing = request.typing && now - heartbeat < 2;
                try {
                    if (!enabled && !typing && !cleanup_needed) {
                        connection.reset();
                        publish(FilterState{false, -1, serial_, now});
                        next = clock_() + .20;
                        continue;
                    }
                    if (!connection)
                        connection = connect_();
                    if (!connection)
                        throw std::runtime_error("Steam UI connection is unavailable");
                    cleanup_needed = cleanup_needed || enabled || typing;
                    auto result = connection->renew({serial_, owner_, enabled, false, false, typing,
                                                     request.touches, request.held,
                                                     request.system_only, request.tap});
                    auto state = confirmation(result.get(), serial_, clock_());
                    cleanup_needed = state.active || keyboard_watch(result.get());
                    publish(std::move(state));
                    if (!cleanup_needed && !enabled && !typing)
                        connection.reset();
                    last_error.clear();
                    next = clock_() + .20;
                } catch (const std::exception &error) {
                    connection.reset();
                    if (last_error != error.what()) {
                        last_error = error.what();
                        publish(FilterError{bounded(last_error)});
                    }
                    next = clock_() + .50;
                }
            }
        }
    } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(mutex_);
        terminal_error_ = bounded(error.what());
    }
    if (!keyboard_only_ && (connection || cleanup_needed)) {
        try {
            if (!connection)
                connection = connect_();
            if (!connection)
                throw std::runtime_error("Steam UI connection is unavailable");
            SteamUiRequest release{serial_, owner_};
            release.system_only = request_.system_only;
            release.tap = request_.tap;
            auto result = connection->renew(release);
            auto state = confirmation(result.get(), serial_, clock_());
            if (state.active || keyboard_watch(result.get()))
                throw std::runtime_error("Steam input filter release was not confirmed");
            publish(std::move(state));
        } catch (const std::exception &error) {
            std::lock_guard<std::mutex> lock(mutex_);
            cleanup_error_ = "Input filter cleanup: " + bounded(error.what());
        }
    }
    connection.reset();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}
void SteamUiClient::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    if (worker_.joinable())
        worker_.join();
    std::optional<std::string> failure;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure = std::exchange(cleanup_error_, std::nullopt);
    }
    if (failure)
        throw std::runtime_error(*failure);
}
} // namespace ct
