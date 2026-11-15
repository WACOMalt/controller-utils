#include "steamos.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <systemd/sd-bus.h>

namespace ct {
SteamOsChannel parse_steam_os_channel(std::string_view branch, std::string_view variant) {
    if (variant != "vr" || (branch != "stable" && branch != "beta"))
        throw std::runtime_error(
            "Controller Utils supports Steam Frame's stable and beta OS channels.");
    return branch == "stable" ? SteamOsChannel::Stable : SteamOsChannel::Beta;
}
SteamOsChannel steam_os_channel() {
    sd_bus *connection = nullptr;
    auto result = sd_bus_open_system(&connection);
    std::unique_ptr<sd_bus, decltype(&sd_bus_unref)> bus(connection, sd_bus_unref);
    if (result < 0)
        throw std::system_error(-result, std::generic_category(), "Read SteamOS update channel");
    result = sd_bus_set_method_call_timeout(bus.get(), 1000000);
    if (result < 0)
        throw std::system_error(-result, std::generic_category(), "Set SteamOS query timeout");
    auto property = [&](const char *name) {
        char *text = nullptr;
        const auto status = sd_bus_get_property_string(
            bus.get(), "com.steampowered.Atomupd1", "/com/steampowered/Atomupd1",
            "com.steampowered.Atomupd1", name, nullptr, &text);
        std::unique_ptr<char, decltype(&std::free)> owned(text, std::free);
        if (status < 0)
            throw std::system_error(-status, std::generic_category(),
                                    "Read SteamOS update channel");
        if (!text || strnlen(text, 65) > 64)
            throw std::runtime_error("SteamOS returned an invalid update channel.");
        return std::string(text);
    };
    const auto variant = property("Variant");
    const auto branch = property("Branch");
    return parse_steam_os_channel(branch, variant);
}
} // namespace ct
