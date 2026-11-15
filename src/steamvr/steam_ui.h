#pragma once
#include "steam_ui_transport.h"
#include "interaction/input_control.h"
#include "platform/curl.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ct {
class SteamUiSession {
  public:
    virtual ~SteamUiSession() = default;
    virtual bool running() const = 0;
    virtual void send(const FilterRequest &request) = 0;
    virtual std::vector<FilterEvent> poll() = 0;
    virtual void close() = 0;
};
using SteamUiFactory = std::function<std::unique_ptr<SteamUiSession>(const std::string &, bool)>;

class SteamUiClient final : public SteamUiSession {
    CurlLibrary library_;
    std::string serial_, owner_;
    bool keyboard_only_;
    SteamUiConnect connect_;
    std::function<double()> clock_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    FilterRequest request_;
    double heartbeat_ = -1e30;
    bool immediate_ = false, stopping_ = false, running_ = true;
    std::vector<FilterEvent> events_;
    std::optional<std::string> terminal_error_, cleanup_error_;
    std::thread worker_;
    void publish(FilterEvent event);
    void run();

  public:
    SteamUiClient(std::string serial, bool keyboard_only = false, SteamUiConnect connect = {},
                  std::function<double()> monotonic = {});
    ~SteamUiClient() override;
    SteamUiClient(const SteamUiClient &) = delete;
    SteamUiClient &operator=(const SteamUiClient &) = delete;
    bool running() const override;
    void send(const FilterRequest &request) override;
    std::vector<FilterEvent> poll() override;
    void close() override;
};
} // namespace ct
