#include "shortcuts.h"
#include "compatibility.h"
#include "module.h"
#include "platform/linux/files.h"
#include "platform/linux/resource.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <sys/uio.h>
#include <unistd.h>

namespace ct {
namespace {
template <class T> T read_state(uintptr_t address) {
    T value{};
    if (!address || address > UINTPTR_MAX - sizeof(value))
        throw std::runtime_error("SteamVR room-view state unavailable");
    iovec local{&value, sizeof(value)}, remote{reinterpret_cast<void *>(address), sizeof(value)};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != sizeof(value))
        throw std::runtime_error("SteamVR room-view state unavailable");
    return value;
}
} // namespace
bool validate_mailbox_table(const steamvr::Module &module, const uintptr_t *table,
                            const std::array<MailboxMethod, 3> &methods) {
    if (!module.type(table, methods.size(), "16CVRMailboxClient"))
        return false;
    for (size_t i = 0; i < methods.size(); ++i) {
        if (!module.method(table[i], methods[i].size, methods[i].signature))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (table[i] < table[j] + methods[j].size && table[j] < table[i] + methods[i].size)
                return false;
    }
    return true;
}
bool validate_progress_table(const steamvr::Module &module, const uintptr_t *table,
                             size_t target_size, const char *target_signature) {
    if (!module.type(table, 25, "16CHmdSystemLatest", -24) ||
        !module.contains(reinterpret_cast<const void *>(table[24]), 8, true))
        return false;
    uint32_t words[2];
    std::memcpy(words, reinterpret_cast<const void *>(table[24]), sizeof(words));
    if (words[0] != 0x9100c000 || (words[1] & 0xfc000000) != 0x14000000)
        return false;
    int64_t delta = words[1] & 0x03ffffff;
    if (delta & 0x02000000)
        delta -= 0x04000000;
    delta *= 4;
    const auto instruction = table[24] + 4;
    if ((delta < 0 && instruction < uintptr_t(-delta)) ||
        (delta >= 0 && uintptr_t(delta) > UINTPTR_MAX - instruction))
        return false;
    const auto target =
        delta < 0 ? instruction - uintptr_t(-delta) : instruction + uintptr_t(delta);
    return module.method(target, target_size, target_signature);
}
OpenVrShortcutBackend::OpenVrShortcutBackend(Runtime &runtime) : runtime_(runtime) {
    try {
        mailbox_ = runtime_.interface("IVRMailbox_002");
    } catch (const std::exception &) {
        throw std::runtime_error("SteamVR mailbox unavailable");
    }
    void *table = nullptr;
    iovec local{&table, sizeof(table)}, remote{mailbox_, sizeof(table)};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != sizeof(table))
        throw std::runtime_error("SteamVR mailbox object unavailable");
    static constexpr std::array<std::array<MailboxMethod, 3>, 2> contracts{
        {{{{1604, "cd32ad99eb7f5f0b8b11e10715b6dd31655bb2b8196d0ef5850f0167da95257f"},
           {908, "a6c96eef5d737a1a239ee958478270cf0986dc671c109f9b9c6e429f276589a9"},
           {588, "b7c872b025ee4240ead009646b2cdaf9c21e92ab6646d9bd263f94d15e529b79"}}},
         {{{1604, "ddf506f2570ee6df53f651eb3fa61926311b8c69579ea9c8a306b3939eb18341"},
           {908, "bed703118b24a3d80d40140d55e8b574be333572f2f2c373aeaab8d49c61bede"},
           {588, "edbf9ae244991d5c06a77b0d367f7738ecb5002d85c77cc52c98db6b43cfa321"}}}}};
    try {
        const auto module = steamvr::Module::loaded(table, steamvr::compatibility::VrClient.path);
        if (!std::any_of(contracts.begin(), contracts.end(), [&](const auto &methods) {
                return validate_mailbox_table(module, static_cast<const uintptr_t *>(table),
                                              methods);
            }))
            throw std::runtime_error("SteamVR mailbox interface changed");
    } catch (const std::runtime_error &) {
        throw std::runtime_error("SteamVR mailbox interface changed");
    }
    mailbox_table_ = static_cast<void **>(table);
}
uint64_t OpenVrShortcutBackend::open(const std::string &name) {
    std::array<char, 256> actual{};
    uint64_t handle = 0;
    const auto function =
        reinterpret_cast<int (*)(void *, const char *, uint64_t *, void *, uint32_t)>(
            mailbox_table_[0]);
    const int result = function(mailbox_, name.c_str(), &handle, actual.data(), actual.size());
    if (result) {
        if (handle) {
            try {
                close(handle);
            } catch (const std::exception &error) {
                std::cerr << error.what() << '\n';
            }
        }
        throw std::runtime_error("could not open app mailbox: " + std::to_string(result));
    }
    return handle;
}
void OpenVrShortcutBackend::send(uint64_t handle, const std::string &target,
                                 const std::string &message) {
    const auto function =
        reinterpret_cast<int (*)(void *, uint64_t, const char *, const char *)>(mailbox_table_[2]);
    const int result = function(mailbox_, handle, target.c_str(), message.c_str());
    if (result)
        throw std::runtime_error("Frame message was not delivered: " + std::to_string(result));
}
void OpenVrShortcutBackend::close(uint64_t handle) {
    const auto function = reinterpret_cast<int (*)(void *, uint64_t)>(mailbox_table_[1]);
    const int result = function(mailbox_, handle);
    if (result)
        throw std::runtime_error("app mailbox did not close: " + std::to_string(result));
}
bool read_roomview_state(const steamvr::Module &module, uintptr_t chaperone) {
    const auto table = read_state<const uintptr_t *>(chaperone);
    if (!module.type(table, 45, "18CVRChaperoneClient") ||
        !module.method(table[44], 32,
                       "9b350ad2786a128f02f899439a44cfb6b2f25834719390138521577afe9d2500"))
        throw std::runtime_error("SteamVR room-view interface changed");
    const auto *words = reinterpret_cast<const uint32_t *>(table[44]);
    const int64_t branch = int64_t(int32_t(words[3] << 6) >> 6) * 4;
    const auto instruction = table[44] + 12;
    if ((words[3] & 0xfc000000) != 0x94000000 || (branch < 0 && instruction < uintptr_t(-branch)) ||
        (branch >= 0 && uintptr_t(branch) > UINTPTR_MAX - instruction))
        throw std::runtime_error("SteamVR room-view accessor changed");
    const auto target =
        branch < 0 ? instruction - uintptr_t(-branch) : instruction + uintptr_t(branch);
    if (!module.method(target, 20,
                       "d4dcc5192e434ee3d91803144048b59d95ff130349a5d749c1343335cc8a9919"))
        throw std::runtime_error("SteamVR room-view provider changed");
    const auto member = (words[1] >> 10) & 0xfff;
    if (chaperone > UINTPTR_MAX - member)
        throw std::runtime_error("SteamVR room-view address overflow");
    const auto provider = read_state<uintptr_t>(chaperone + member);
    const auto provider_table = read_state<const uintptr_t *>(provider);
    if (!module.type(provider_table, 2, "N3IPC17PosixSharedMemoryE") ||
        !module.method(provider_table[1], 8,
                       "dfb556a413c8bda5332942d0202e8d5f1e52f45158aec3676294cf11ad5909cb"))
        throw std::runtime_error("SteamVR room-view mapping changed");
    const auto load = *reinterpret_cast<const uint32_t *>(provider_table[1]);
    const auto mapping_member = ((load >> 10) & 0xfff) * 8;
    if (provider > UINTPTR_MAX - mapping_member)
        throw std::runtime_error("SteamVR room-view address overflow");
    const auto state = read_state<uintptr_t>(provider + mapping_member);
    const auto offset = (((words[4] >> 10) & 0xfff) << 12) + ((words[6] >> 10) & 0xfff);
    if (!state || state > UINTPTR_MAX - offset)
        throw std::runtime_error("SteamVR room-view state unavailable");
    const auto enabled = read_state<uint8_t>(state + offset);
    if (enabled > 1 || table != read_state<const uintptr_t *>(chaperone) ||
        provider != read_state<uintptr_t>(chaperone + member) ||
        provider_table != read_state<const uintptr_t *>(provider) ||
        state != read_state<uintptr_t>(provider + mapping_member))
        throw std::runtime_error("SteamVR room-view mapping changed while reading");
    return enabled;
}
bool OpenVrShortcutBackend::roomview_enabled() {
    const auto chaperone = reinterpret_cast<uintptr_t>(runtime_.interface("IVRChaperone_004"));
    const auto table = read_state<const uintptr_t *>(chaperone);
    return read_roomview_state(
        steamvr::Module::loaded(table, steamvr::compatibility::VrClient.path), chaperone);
}
void OpenVrShortcutBackend::prepare_progress() {
    if (client_)
        return;
    void *client = nullptr;
    try {
        client = runtime_.interface("IVRClientInternal_XXX");
    } catch (const std::exception &) {
        throw std::runtime_error("SteamVR shortcut animations unavailable");
    }
    void *table_address = nullptr;
    iovec local{&table_address, sizeof(table_address)}, remote{client, sizeof(table_address)};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != sizeof(table_address))
        throw std::runtime_error("SteamVR shortcut animation object unavailable");
    const auto table = static_cast<const uintptr_t *>(table_address);
    try {
        const auto module = steamvr::Module::loaded(table, steamvr::compatibility::VrClient.path);
        if (!validate_progress_table(
                module, table, 180,
                "c166aa2b8ac8ab97ea0593ef162c3c967b85779c2ed209d2a8bedaca48dd55a0"))
            throw std::runtime_error("invalid animation method");
    } catch (const std::runtime_error &) {
        throw std::runtime_error("SteamVR shortcut animation interface changed");
    }
    const std::array<const char *, 4> names{"/user/head", "/input/system",
                                            "/actions/quickrecenter/in/recenter",
                                            "/actions/controllerutils/in/passthrough"};
    std::array<uint64_t, 4> handles{};
    for (size_t i = 0; i < names.size(); ++i)
        handles[i] = runtime_.path_handle(names[i]);
    progress_paths_ = handles;
    post_ = reinterpret_cast<decltype(post_)>(table[24]);
    client_ = client;
}
void OpenVrShortcutBackend::post_progress(const ShortcutProgress &event) {
    if (!client_ || !post_)
        throw std::logic_error("shortcut progress is not prepared");
    static_assert(sizeof(vr::VREvent_t) == 60 && offsetof(vr::VREvent_t, data) == 12);
    vr::VREvent_t packet{};
    packet.eventType = 1705;
    packet.data.progressUpdate.pathDevice = progress_paths_[0];
    packet.data.progressUpdate.pathInputSource = progress_paths_[1];
    packet.data.progressUpdate.pathProgressAction =
        progress_paths_[event.action == ShortcutAction::Recenter ? 2 : 3];
    packet.data.progressUpdate.fProgress = float(event.amount);
    post_(client_, &packet, 0);
}
NativeShortcuts::NativeShortcuts(ShortcutBackend &backend, std::function<double()> monotonic,
                                 std::string group)
    : backend_(backend), monotonic_(monotonic ? std::move(monotonic) : monotonic_time) {
    if (group.empty())
        group = "controllerutils." + std::to_string(getpid());
    handle_ = backend_.open(group);
}
NativeShortcuts::~NativeShortcuts() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
}
void NativeShortcuts::send_action(ShortcutAction action) {
    if (!handle_)
        throw std::logic_error("shortcut mailbox is closed");
    std::string message;
    if (action == ShortcutAction::Passthrough)
        message = std::string(R"({"type":"set_camera_roomview_enabled","enabled":)") +
                  (backend_.roomview_enabled() ? "false}" : "true}");
    else if (action == ShortcutAction::Recenter)
        message = R"({"type":"seated_and_standing_position_reset"})";
    else
        throw std::invalid_argument("unsupported Frame shortcut");
    backend_.send(handle_, "vrcompositor_systemlayer", message);
}
void NativeShortcuts::progress(std::optional<ShortcutAction> action, double amount) {
    if ((action && *action != ShortcutAction::Recenter && *action != ShortcutAction::Passthrough) ||
        !std::isfinite(amount) || amount < 0 || amount > 1 || (!action && amount != 0))
        throw std::invalid_argument("invalid native shortcut progress");
    if (!action && !progress_action_)
        return;
    if (!progress_ready_) {
        backend_.prepare_progress();
        progress_ready_ = true;
    }
    const double now = monotonic_();
    if (action == progress_action_ && amount != 0 && now - progress_last_ < .05)
        return;
    if (progress_action_ && action != progress_action_)
        backend_.post_progress({*progress_action_, 0});
    if (action)
        backend_.post_progress({*action, amount});
    progress_action_ = action;
    progress_last_ = now;
}
void NativeShortcuts::close() {
    std::exception_ptr failure;
    try {
        progress(std::nullopt);
    } catch (...) {
        failure = std::current_exception();
    }
    const auto handle = std::exchange(handle_, 0);
    if (handle) {
        try {
            backend_.close(handle);
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    }
    if (failure)
        std::rethrow_exception(failure);
}
} // namespace ct
