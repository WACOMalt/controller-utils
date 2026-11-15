#pragma once

namespace ct::steamvr::compatibility {
struct Binary {
    const char *name, *path;
    const char *sha256 = nullptr;
};
inline constexpr Binary XrService{
    "XRService", "/opt/steamvr/drivers/cv/bin/linuxarm64/XRService",
    "a853698ae128bbeabfc1c915af2f96477686903d7e9155c41df92b5184abde47"};
inline constexpr Binary VrClient{"vrclient.so", "/opt/steamvr/bin/linuxarm64/vrclient.so"};
inline constexpr Binary Compositor{"vrcompositor", "/opt/steamvr/bin/linuxarm64/vrcompositor"};
inline constexpr Binary SteamWebHelper{
    "steamwebhelper", "/home/steamos/.local/share/Steam/steamrtarm64/steamwebhelper"};
} // namespace ct::steamvr::compatibility
