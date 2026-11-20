#include "steamvr/ui_gate.h"
#include "platform/linux/files.h"
#include "platform/linux/process.h"
#include "platform/linux/resource.h"
#include "steamvr/compatibility.h"
#include "steamvr/optical_socket.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <thread>

#if defined(__aarch64__)
namespace {
using ct::FileDescriptor;

class Gate {
    FileDescriptor image{open("/proc/self/mem", O_RDWR | O_CLOEXEC)};
    FileDescriptor listener;
    uintptr_t index_address = 0;
    int32_t index = -1;
    int32_t limit = 0;
    bool acknowledge = false;

    void write(uintptr_t address, const void *bytes, size_t size) {
        ssize_t count;
        do {
            count = pwrite(image.get(), bytes, size, address);
        } while (count < 0 && errno == EINTR);
        if (count != static_cast<ssize_t>(size))
            throw std::runtime_error("could not update Steam UI input gate");
    }

    void set_index(int32_t next) {
        if (next == index)
            return;
        write(index_address, &next, sizeof(next));
        index = next;
    }

  public:
    Gate(ct::steamvr::GateKind kind, const char *executable) {
        const auto bytes = ct::read_file("/proc/self/exe", 32 * 1024 * 1024);
        const auto gate = ct::steamvr::resolve_gate_layout(kind, bytes);
        limit = gate.index_limit;
        acknowledge = kind == ct::steamvr::GateKind::Input;
        const auto maps = ct::process_mappings(ct::read_file("/proc/self/maps", 1024 * 1024));
        const auto mapping = std::find_if(maps.begin(), maps.end(), [&](const auto &item) {
            return item.path == executable && item.offset == 0;
        });
        if (!image || mapping == maps.end())
            throw std::runtime_error("Steam UI executable mapping unavailable");
        const uintptr_t base = mapping->first;
        ct::steamvr::validate_gate_mappings(gate, base, maps);
        const auto *entry = reinterpret_cast<const char *>(base + gate.site);
        auto *cave = reinterpret_cast<char *>(base + gate.cave);
        auto original = gate.original;
        original.insert(original.end(), gate.following.begin(), gate.following.end());
        if (memcmp(entry, original.data(), original.size()) ||
            std::any_of(cave, cave + gate.payload.size(), [](char byte) { return byte != 0; }))
            throw std::runtime_error("Steam UI input gate site is occupied");

        listener.reset(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        sockaddr_un address{};
        const auto length = ct::optical::address(address, acknowledge ? "input" : "beam");
        if (!listener ||
            bind(listener.get(), reinterpret_cast<const sockaddr *>(&address), length) ||
            listen(listener.get(), 1))
            throw std::runtime_error("could not listen for Steam UI input leases");

        // The target has not entered main yet. Publish the complete, disabled gate first.
        write(base + gate.cave, gate.payload.data(), gate.payload.size());
        __builtin___clear_cache(cave, cave + gate.payload.size());
        write(base + gate.site, gate.redirect.data(), gate.redirect.size());
        __builtin___clear_cache(const_cast<char *>(entry),
                                const_cast<char *>(entry) + gate.redirect.size());
        index_address = base + gate.cave + gate.index_offset;
    }

    void run() {
        FileDescriptor peer;
        double last = 0;
        while (true) {
            pollfd status{peer ? peer.get() : listener.get(), POLLIN, 0};
            if (poll(&status, 1, 50) < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (!peer) {
                peer.reset(accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
                if (peer && !ct::optical::same_user(peer.get()))
                    peer.reset();
                last = ct::monotonic_time();
                continue;
            }
            if (status.revents & POLLIN) {
                int32_t next;
                const auto count = recv(peer.get(), &next, sizeof(next), MSG_TRUNC);
                if (count == sizeof(next) && next >= -1 && next < limit) {
                    set_index(next);
                    if (!acknowledge ||
                        send(peer.get(), &next, sizeof(next), MSG_NOSIGNAL) == sizeof(next)) {
                        last = ct::monotonic_time();
                        continue;
                    }
                }
            }
            if (status.revents || ct::monotonic_time() - last > .60) {
                set_index(-1);
                peer.reset();
            }
        }
        set_index(-1);
    }

    void reset() {
        set_index(-1);
    }
};

__attribute__((constructor)) void start() {
    char executable[1024]{};
    if (readlink("/proc/self/exe", executable, sizeof(executable) - 1) <= 0)
        return;
    const bool beam = !strcmp(executable, ct::steamvr::compatibility::Compositor.path);
    if (!beam && strcmp(executable, ct::steamvr::compatibility::SteamWebHelper.path))
        return;
    const char *name = beam ? "beam" : "input";
    try {
        if (!beam &&
            ct::read_file("/proc/self/cmdline", 65536).find("--type=") != std::string::npos)
            return;
        auto gate = std::make_unique<Gate>(
            beam ? ct::steamvr::GateKind::Beam : ct::steamvr::GateKind::Input, executable);
        std::thread([gate = std::move(gate), name] {
            try {
                gate->run();
            } catch (const std::exception &error) {
                dprintf(2, "Controller Utils %s gate: %s\n", name, error.what());
                try {
                    gate->reset();
                } catch (const std::exception &) {
                }
            }
        }).detach();
        dprintf(2, "Controller Utils %s gate: ready\n", name);
    } catch (const std::exception &error) {
        dprintf(2, "Controller Utils %s gate: %s\n", name, error.what());
    }
}
} // namespace
#endif
