#pragma once
#include <array>
#include <string_view>

namespace ct::steamvr::compatibility {
struct Binary {
    const char *name, *path;
};
inline constexpr Binary XrService{"XRService", "/opt/steamvr/drivers/cv/bin/linuxarm64/XRService"};
inline constexpr std::array<const char *, 2> XrServiceHashes{
    "a853698ae128bbeabfc1c915af2f96477686903d7e9155c41df92b5184abde47",
    "9e1c6f57a7e3613c69dbf7f61c62d0cfd4b8a664889791907772b03a62f9f6d8"};
inline bool supported_xr_service(std::string_view fingerprint) {
    for (const auto approved : XrServiceHashes)
        if (fingerprint == approved)
            return true;
    return false;
}
inline constexpr Binary VrClient{"vrclient.so", "/opt/steamvr/bin/linuxarm64/vrclient.so"};
inline constexpr Binary Compositor{"vrcompositor", "/opt/steamvr/bin/linuxarm64/vrcompositor"};
inline constexpr Binary SteamWebHelper{
    "steamwebhelper", "/home/steamos/.local/share/Steam/steamrtarm64/steamwebhelper"};
} // namespace ct::steamvr::compatibility
