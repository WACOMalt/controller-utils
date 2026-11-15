#include "discovery.h"
#include "platform/linux/error.h"
#include "platform/linux/files.h"
#include "platform/linux/process.h"
#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>

namespace ct::helper {
pid_t find_process(const steamvr::compatibility::Binary &binary, bool require_vr, uid_t owner) {
    std::vector<pid_t> matches;
    for (const auto &entry : std::filesystem::directory_iterator("/proc")) {
        const auto name = entry.path().filename().string();
        int pid = 0;
        const auto number = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (number.ec != std::errc{} || number.ptr != name.data() + name.size() || pid < 2)
            continue;
        try {
            struct stat info{};
            if (stat(entry.path().c_str(), &info) || info.st_uid != owner ||
                std::filesystem::read_symlink(entry.path() / "exe") != binary.path)
                continue;
            if (std::string(binary.name) == "XRService" &&
                read_file(entry.path() / "comm", 1024).rfind("XRService", 0) != 0)
                continue;
            if (require_vr) {
                const auto maps = stable_process_mappings(
                    [&] { return read_file(entry.path() / "maps", 4 * 1024 * 1024); });
                bool linked = false;
                for (const auto &mapping : maps)
                    if (std::filesystem::path(mapping.path).filename() == "vrclient.so")
                        linked = true;
                if (!linked)
                    continue;
            }
            matches.push_back(pid);
        } catch (const std::system_error &error) {
            const auto code = error.code().value();
            if (code != ENOENT && code != ESRCH && code != EACCES && code != EPERM)
                throw;
        }
    }
    if (matches.size() != 1) {
        const auto message = std::string(binary.name) == "XRService"
                                 ? "Expected exactly one stock Frame tracking service."
                             : require_vr ? "Expected one Steam UI process connected to VR."
                                          : "Expected one native SteamVR compositor.";
        if (matches.empty())
            throw ConnectionError(message);
        throw std::runtime_error(message);
    }
    return matches.front();
}
} // namespace ct::helper
