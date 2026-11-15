#pragma once
#include "platform/json.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ct {
namespace steamui {
bool serial_valid(const std::string &serial);
}
struct SteamUiRequest {
    std::string serial, owner;
    bool enabled = false, keyboard = false, action_only = false, typing = false;
    int touches = 0;
    bool held = false;
    bool system_only = false;
    uint32_t tap = 0;
};
class SteamUiConnection {
  public:
    virtual ~SteamUiConnection() = default;
    virtual json::Value renew(const SteamUiRequest &request) = 0;
};
using SteamUiConnect = std::function<std::unique_ptr<SteamUiConnection>()>;
std::unique_ptr<SteamUiConnection> connect_steam_ui();
} // namespace ct
