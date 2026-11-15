#include "tracker_control.h"
#include "adapter_patch.h"
#include <array>
#include <limits>
#include <stdexcept>

namespace ct::helper {
namespace {
uint64_t deadline(uint64_t now) {
    if (!now || now > UINT64_MAX - 1500000000ULL)
        throw std::invalid_argument("invalid native lease clock");
    return now + 1500000000ULL;
}
} // namespace
tracker::Control TrackerControl::state() {
    const auto bytes = memory_.read(AdapterPatch::Control, sizeof(tracker::Control));
    if (bytes.size() != sizeof(tracker::Control))
        throw std::runtime_error("short native adapter state");
    tracker::Control state;
    std::memcpy(&state, bytes.data(), sizeof(state));
    if (state.magic != tracker::Magic || state.version != 1)
        throw std::runtime_error("Isolated native control identity mismatch.");
    if (state.command > 1 || state.phase > tracker::Failed || state.active > 4096 ||
        state.loader_started > 1 || state.worker_exited > 1)
        throw std::runtime_error("invalid native adapter state");
    for (const auto pointer :
         {state.wrapper, state.manager, state.classic, state.stock_wrapper, state.callbacks})
        if (pointer % 8)
            throw std::runtime_error("unaligned native adapter pointer");
    return state;
}
uint32_t TrackerControl::begin(uint64_t raw_ns) {
    const auto until = deadline(raw_ns);
    const auto before = state();
    if (before.command || before.wrapper || before.manager || before.classic || before.active ||
        (before.phase != tracker::Empty && before.phase != tracker::Stopped &&
         before.phase != tracker::Failed))
        throw std::runtime_error(
            "resident adapter is active or not safely reusable. Restart SteamVR");
    if (before.error == 2 || before.error == 3 || before.worker_exited)
        throw std::runtime_error("Private native worker cannot restart. Restart SteamVR");
    if (before.generation == UINT32_MAX)
        throw std::runtime_error("Native generation exhausted. Restart SteamVR.");
    generation_ = before.generation + 1;
    const std::array<uint32_t, 2> phase{};
    write(112, phase);
    write(124, *generation_);
    write(16, until);
    write(12, uint32_t(1));
    return *generation_;
}
void TrackerControl::renew(uint64_t raw_ns) {
    const auto until = deadline(raw_ns);
    if (!generation_)
        throw std::logic_error("native tracking generation has not started");
    const auto current = state();
    if (current.generation != *generation_)
        throw std::runtime_error("native tracking generation changed");
    write(16, until);
}
void TrackerControl::request_stop() {
    state();
    write(12, uint32_t(0));
}
bool TrackerControl::stopped() {
    const auto current = state();
    return !current.command && !current.wrapper && !current.manager && !current.classic &&
           !current.active &&
           (current.phase == tracker::Empty || current.phase == tracker::Stopped ||
            current.phase == tracker::Failed);
}
json::Value tracker_status(const tracker::Control &state) {
    auto result = json::parse("{}");
    const auto value = result.get();
    const std::pair<const char *, uint64_t> fields[]{{"command", state.command},
                                                     {"deadline", state.deadline_raw_ns},
                                                     {"wrapper", state.wrapper},
                                                     {"manager", state.manager},
                                                     {"classic", state.classic},
                                                     {"phase", state.phase},
                                                     {"error", state.error},
                                                     {"active", state.active},
                                                     {"generation", state.generation},
                                                     {"frames", state.frames},
                                                     {"imu", state.imu},
                                                     {"max_frame_ns", state.max_frame_ns}};
    for (const auto &[name, number] : fields)
        json_object_object_add(value, name, json_object_new_uint64(number));
    return result;
}
} // namespace ct::helper
