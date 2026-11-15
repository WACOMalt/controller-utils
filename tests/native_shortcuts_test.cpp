#include "steamvr/shortcuts.h"
#include "steamvr/module.h"
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
class Backend final : public ct::ShortcutBackend {
  public:
    std::vector<std::string> events;
    bool roomview = false, fail_progress = false, fail_prepare = false, fail_close = false;
    unsigned prepared = 0, closed = 0;
    uint64_t open(const std::string &name) override {
        events.push_back("open:" + name);
        return 42;
    }
    void send(uint64_t handle, const std::string &target, const std::string &message) override {
        assert(handle == 42 && target == "vrcompositor_systemlayer");
        events.push_back(message);
    }
    void close(uint64_t handle) override {
        assert(handle == 42);
        ++closed;
        events.push_back("close");
        if (fail_close)
            throw std::runtime_error("close failed");
    }
    bool roomview_enabled() override {
        return roomview;
    }
    void prepare_progress() override {
        ++prepared;
        if (fail_prepare)
            throw std::runtime_error("animation unavailable");
    }
    void post_progress(const ct::ShortcutProgress &event) override {
        events.push_back(
            (event.action == ct::ShortcutAction::Recenter ? "recenter:" : "passthrough:") +
            std::to_string(event.amount));
        if (fail_progress)
            throw std::runtime_error("progress failed");
    }
};
template <class Function> void rejects(Function function, const std::string &expected) {
    bool failed = false;
    try {
        function();
    } catch (const std::exception &error) {
        failed = error.what() == expected;
    }
    assert(failed);
}
} // namespace

int main() {
    {
        struct Fixture {
            char name[19] = "16CVRMailboxClient";
            uintptr_t type[2]{};
            uintptr_t table[5]{};
            uint32_t code[3]{0xd503201f, 0xd65f03c0, 0x52800000};
        } fixture;
        fixture.type[1] = reinterpret_cast<uintptr_t>(fixture.name);
        fixture.table[1] = reinterpret_cast<uintptr_t>(fixture.type);
        for (size_t i = 0; i < 3; ++i)
            fixture.table[i + 2] = reinterpret_cast<uintptr_t>(&fixture.code[i]);
        const auto begin = reinterpret_cast<uintptr_t>(&fixture);
        ct::steamvr::Module module({{begin, begin + sizeof(fixture), true}});
        const std::array<ct::MailboxMethod, 3> methods{{
            {4, "04ca88f2b88d606239021d6eb03752f117e3f73fb022df93dbe99ab93edf368b"},
            {4, "110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91"},
            {4, "c6aeef4904b1a8fa4d188c9dfbb1a5fe55a0d12799de57cec735ace9a6d8baa8"},
        }};
        assert(ct::validate_mailbox_table(module, fixture.table + 2, methods));
        fixture.table[2] = fixture.table[3];
        assert(!ct::validate_mailbox_table(module, fixture.table + 2, methods));
        fixture.table[2] = reinterpret_cast<uintptr_t>(&fixture.code[0]);
        fixture.code[1] ^= 1;
        assert(!ct::validate_mailbox_table(module, fixture.table + 2, methods));
        fixture.code[1] ^= 1;
        fixture.name[2] = 'X';
        assert(!ct::validate_mailbox_table(module, fixture.table + 2, methods));
    }
    {
        struct Fixture {
            char name[19] = "16CHmdSystemLatest";
            uintptr_t type[2]{};
            uintptr_t table[27]{};
            uint32_t thunk[2]{0x9100c000, 0x14000001};
            uint32_t target[2]{0xd65f03c0, 0xd503201f};
        } fixture;
        fixture.type[1] = reinterpret_cast<uintptr_t>(fixture.name);
        fixture.table[0] = uintptr_t(-24);
        fixture.table[1] = reinterpret_cast<uintptr_t>(fixture.type);
        fixture.table[26] = reinterpret_cast<uintptr_t>(fixture.thunk);
        const auto begin = reinterpret_cast<uintptr_t>(&fixture);
        ct::steamvr::Module module({{begin, begin + sizeof(fixture), true}});
        const char *digest = "110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91";
        assert(ct::validate_progress_table(module, fixture.table + 2, 4, digest));
        fixture.thunk[1] = 0x14000002;
        assert(!ct::validate_progress_table(module, fixture.table + 2, 4, digest));
        fixture.thunk[1] = 0x14000001;
        fixture.target[0] ^= 1;
        assert(!ct::validate_progress_table(module, fixture.table + 2, 4, digest));
    }
    for (const bool stable : {false, true}) {
        struct Fixture {
            char chaperone_name[22] = "18CVRChaperoneClient";
            char provider_name[28] = "N3IPC17PosixSharedMemoryE";
            uintptr_t chaperone_type[2]{}, provider_type[2]{};
            uintptr_t table[47]{}, provider_table[4]{};
            uintptr_t chaperone[29]{}, provider[3]{};
            uint32_t accessor[8]{0xa9bf7bfd, 0x91038000, 0x910003fd, 0x94000005,
                                 0x91410c00, 0xa8c17bfd, 0x397d1000, 0xd65f03c0};
            uint32_t helper[5]{0xf9400000, 0xf9400001, 0xf9400421, 0xaa0103f0, 0xd61f0200};
            uint32_t getter[2]{0xf9400800, 0xd65f03c0};
        } fixture;
        if (stable) {
            fixture.accessor[4] = 0x91410000;
            fixture.accessor[6] = 0x396d1000;
        }
        fixture.chaperone_type[1] = reinterpret_cast<uintptr_t>(fixture.chaperone_name);
        fixture.provider_type[1] = reinterpret_cast<uintptr_t>(fixture.provider_name);
        fixture.table[1] = reinterpret_cast<uintptr_t>(fixture.chaperone_type);
        fixture.provider_table[1] = reinterpret_cast<uintptr_t>(fixture.provider_type);
        fixture.table[46] = reinterpret_cast<uintptr_t>(fixture.accessor);
        fixture.provider_table[3] = reinterpret_cast<uintptr_t>(fixture.getter);
        fixture.chaperone[0] = reinterpret_cast<uintptr_t>(fixture.table + 2);
        fixture.chaperone[28] = reinterpret_cast<uintptr_t>(fixture.provider);
        fixture.provider[0] = reinterpret_cast<uintptr_t>(fixture.provider_table + 2);
        std::vector<uint8_t> state(stable ? 0x40b45 : 0x43f45);
        fixture.provider[2] = reinterpret_cast<uintptr_t>(state.data());
        const auto begin = reinterpret_cast<uintptr_t>(&fixture);
        const auto object = reinterpret_cast<uintptr_t>(fixture.chaperone);
        ct::steamvr::Module module({{begin, begin + sizeof(fixture), true}});
        assert(!ct::read_roomview_state(module, object));
        state.back() = 1;
        assert(ct::read_roomview_state(module, object));
        state.back() = 2;
        rejects([&] { ct::read_roomview_state(module, object); },
                "SteamVR room-view mapping changed while reading");
        state.back() = 0;
        fixture.provider[2] = 1;
        rejects([&] { ct::read_roomview_state(module, object); },
                "SteamVR room-view state unavailable");
        fixture.provider[2] = 0;
        rejects([&] { ct::read_roomview_state(module, object); },
                "SteamVR room-view state unavailable");
        fixture.getter[0] ^= 1;
        rejects([&] { ct::read_roomview_state(module, object); },
                "SteamVR room-view mapping changed");
        fixture.getter[0] ^= 1;
        fixture.helper[0] ^= 1;
        rejects([&] { ct::read_roomview_state(module, object); },
                "SteamVR room-view provider changed");
        fixture.helper[0] ^= 1;
        fixture.accessor[1] ^= 1;
        rejects([&] { ct::read_roomview_state(module, object); },
                "SteamVR room-view interface changed");
        rejects([&] { ct::read_roomview_state(module, 1); }, "SteamVR room-view state unavailable");
    }
    using Action = ct::ShortcutAction;
    Backend backend;
    double now = 100;
    ct::NativeShortcuts shortcuts(backend, [&] { return now; }, "test");
    assert(backend.events == std::vector<std::string>{"open:test"});
    shortcuts.send_action(Action::Recenter);
    shortcuts.send_action(Action::Passthrough);
    backend.roomview = true;
    shortcuts.send_action(Action::Passthrough);
    assert(backend.events[1] == R"({"type":"seated_and_standing_position_reset"})");
    assert(backend.events[2] == R"({"type":"set_camera_roomview_enabled","enabled":true})");
    assert(backend.events[3] == R"({"type":"set_camera_roomview_enabled","enabled":false})");
    shortcuts.progress(std::nullopt);
    assert(!backend.prepared);
    shortcuts.progress(Action::Recenter, .5);
    assert(backend.prepared == 1 && backend.events.back() == "recenter:0.500000");
    const auto count = backend.events.size();
    now += .01;
    shortcuts.progress(Action::Recenter, .6);
    assert(backend.events.size() == count);
    shortcuts.progress(Action::Passthrough, .1);
    assert(backend.events[backend.events.size() - 2] == "recenter:0.000000");
    assert(backend.events.back() == "passthrough:0.100000");
    now += .06;
    shortcuts.progress(Action::Passthrough, .2);
    assert(backend.events.back() == "passthrough:0.200000");
    shortcuts.progress(Action::Passthrough, 0);
    assert(backend.events.back() == "passthrough:0.000000");
    for (double invalid : {-1., 1.1, std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { shortcuts.progress(Action::Recenter, invalid); },
                "invalid native shortcut progress");
    rejects([&] { shortcuts.progress(std::nullopt, .1); }, "invalid native shortcut progress");
    shortcuts.close();
    assert(backend.events[backend.events.size() - 2] == "passthrough:0.000000");
    assert(backend.events.back() == "close" && backend.closed == 1 && backend.prepared == 1);
    shortcuts.close();
    assert(backend.closed == 1);
    rejects([&] { shortcuts.send_action(Action::Recenter); }, "shortcut mailbox is closed");
    {
        Backend failed;
        failed.fail_prepare = true;
        ct::NativeShortcuts owner(failed, [&] { return now; }, "test");
        rejects([&] { owner.progress(Action::Recenter, .5); }, "animation unavailable");
        owner.send_action(Action::Recenter);
        assert(failed.events.back() == R"({"type":"seated_and_standing_position_reset"})");
        owner.send_action(Action::Passthrough);
        assert(failed.events.back() == R"({"type":"set_camera_roomview_enabled","enabled":true})");
    }
    {
        Backend failed;
        ct::NativeShortcuts owner(failed, [&] { return now; }, "test");
        owner.progress(Action::Recenter, .5);
        failed.fail_progress = failed.fail_close = true;
        rejects([&] { owner.close(); }, "progress failed");
        assert(failed.closed == 1);
        failed.fail_progress = false;
    }
}
