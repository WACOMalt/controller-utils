#pragma once

namespace ct::steamvr::compatibility {
struct Binary {
    const char *name, *path;
};
inline constexpr Binary VrClient{"vrclient.so", "/opt/steamvr/bin/linuxarm64/vrclient.so"};
inline constexpr Binary Compositor{"vrcompositor", "/opt/steamvr/bin/linuxarm64/vrcompositor"};
inline constexpr Binary SteamWebHelper{
    "steamwebhelper", "/home/steamos/.local/share/Steam/steamrtarm64/steamwebhelper"};
} // namespace ct::steamvr::compatibility
