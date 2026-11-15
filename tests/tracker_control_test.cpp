#include "helper/tracker_control.h"
#include "patch_memory.h"
#include <cassert>
#include <cstring>
#include <iostream>

namespace {
void seed(ct::test::PatchMemory &memory, ct::tracker::Control state = {}) {
    state.magic = ct::tracker::Magic;
    state.version = 1;
    ct::helper::Bytes bytes(sizeof(state));
    std::memcpy(bytes.data(), &state, sizeof(state));
    memory.regions[ct::helper::AdapterPatch::Control] = bytes;
}
template <class Function> void rejects(Function fn, const std::string &text) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(text) != std::string::npos;
    }
    assert(failed);
}
} // namespace
int main() {
    using namespace ct::helper;
    constexpr auto address = AdapterPatch::Control;
    ct::test::PatchMemory memory;
    ct::tracker::Control loaded{};
    loaded.loader_started = 1;
    seed(memory, loaded);
    TrackerControl control(memory);
    rejects([&] { control.renew(1); }, "has not started");
    for (uint32_t generation = 1; generation <= 100; ++generation) {
        assert(control.begin(100) == generation);
        const auto state = control.state();
        assert(state.generation == generation && state.command == 1 &&
               state.deadline_raw_ns == 1500000100);
        assert(state.phase == 0 && state.error == 0 && !control.stopped());
        assert(state.loader_started == 1 && !state.worker_exited);
        const auto count = memory.addresses.size();
        assert((std::vector<uintptr_t>(memory.addresses.end() - 4, memory.addresses.end()) ==
                std::vector<uintptr_t>{address + 112, address + 124, address + 16, address + 12}));
        control.renew(200);
        assert(memory.addresses.size() == count + 1 &&
               control.state().deadline_raw_ns == 1500000200);
        control.request_stop();
        assert(control.stopped());
    }
    {
        auto state = control.state();
        state.phase = ct::tracker::Ready;
        state.manager = 0x1000;
        state.active = 1;
        state.command = 1;
        seed(memory, state);
        rejects([&] { control.begin(1); }, "not safely reusable");
        control.request_stop();
        assert(!control.stopped() && control.state().manager == 0x1000);
        state.command = 0;
        state.manager = state.active = 0;
        state.phase = ct::tracker::Stopped;
        seed(memory, state);
        assert(control.stopped());
    }
    for (const uint32_t error : {2, 3}) {
        ct::tracker::Control state{};
        state.phase = ct::tracker::Failed;
        state.error = error;
        seed(memory, state);
        const auto count = memory.writes;
        rejects([&] { control.begin(1); }, "cannot restart");
        assert(memory.writes == count);
    }
    {
        ct::tracker::Control state{};
        state.phase = ct::tracker::Failed;
        state.worker_exited = 1;
        seed(memory, state);
        const auto writes = memory.writes;
        rejects([&] { control.begin(1); }, "cannot restart");
        assert(memory.writes == writes);
    }
    for (bool after : {false, true}) {
        for (unsigned failed = 1; failed <= 4; ++failed) {
            ct::test::PatchMemory broken;
            seed(broken);
            broken.fail_at = failed;
            broken.fail_after = after;
            TrackerControl owner(broken);
            rejects([&] { owner.begin(1); }, "injected patch write failure");
            owner.request_stop();
            assert(owner.stopped());
        }
    }
    {
        ct::tracker::Control state{};
        state.generation = UINT32_MAX;
        seed(memory, state);
        rejects([&] { control.begin(1); }, "exhausted");
        seed(memory);
        const auto count = memory.writes;
        rejects([&] { control.begin(0); }, "clock");
        rejects([&] { control.begin(UINT64_MAX); }, "clock");
        assert(memory.writes == count);
        control.begin(1);
        state = control.state();
        ++state.generation;
        seed(memory, state);
        rejects([&] { control.renew(1); }, "generation changed");
        state.command = 2;
        seed(memory, state);
        rejects([&] { control.state(); }, "invalid native adapter state");
        seed(memory);
        memory.regions[address][0] ^= 1;
        rejects([&] { control.request_stop(); }, "identity mismatch");
    }
    {
        ct::tracker::Control state{};
        state.wrapper = 0x1000;
        state.manager = 0x2000;
        state.classic = 0x3000;
        state.frames = UINT64_MAX;
        state.max_frame_ns = 1234;
        seed(memory, state);
        const auto output = tracker_status(control.state());
        assert(json_object_object_length(output.get()) == 12);
        assert(ct::json::unsigned_integer(ct::json::field(output.get(), "frames")) == UINT64_MAX);
        assert(ct::json::unsigned_integer(ct::json::field(output.get(), "wrapper")) == 0x1000);
    }
    std::cout << "native control: 100 generations, lease ordering, every write failure, terminal "
                 "workers and ownership passed\n";
}
