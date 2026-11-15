#include "platform/linux/steamos.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using namespace ct;
    assert(parse_steam_os_channel("stable", "vr") == SteamOsChannel::Stable);
    assert(parse_steam_os_channel("beta", "vr") == SteamOsChannel::Beta);
    for (const auto branch :
         {"preview", "main", "rc", "bc", "pc", "staging", "rel", "", "Beta", "beta\n"}) {
        bool refused = false;
        try {
            parse_steam_os_channel(branch, "vr");
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);
    }
    for (const auto variant : {"", "steamdeck", "deck", "vr\n"}) {
        bool refused = false;
        try {
            parse_steam_os_channel("stable", variant);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);
    }
    std::cout << "SteamOS channels: stable and beta only, Frame variant and exact names passed\n";
}
