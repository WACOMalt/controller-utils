#pragma once
#include <string_view>

namespace ct {
enum class SteamOsChannel { Stable, Beta };
SteamOsChannel parse_steam_os_channel(std::string_view branch, std::string_view variant);
SteamOsChannel steam_os_channel();
} // namespace ct
