#include "xr_process.h"
#include <stdexcept>

namespace ct::helper {
XrProcess::XrProcess(PinnedProcess &process, NativePackage *package,
                     CapabilityPublisher *capability)
    : process_(process), memory_(process), package_(package), capability_(capability) {
    if (!package_ || !capability_)
        throw std::invalid_argument("incomplete private tracker ownership");
    refresh();
    if (package_)
        package_->verify(process_.mappings());
}
void XrProcess::refresh() {
    process_.refresh();
}
Bytes XrProcess::read(uintptr_t address, size_t size) {
    return memory_.read(address, size);
}
void XrProcess::data(uintptr_t address, const Bytes &value) {
    memory_.data(address, value);
}
void XrProcess::code(uintptr_t address, const Bytes &value) {
    memory_.code(address, value);
}
PatchResult XrProcess::patch(TrackerPatch action) {
    refresh();
    if (action == TrackerPatch::VerifyOriginal) {
        if (!package_)
            throw std::logic_error("private tracker package is unavailable");
        for (const auto &site : package_->layout().sites)
            if (memory_.read(site.address, 16) != site.original)
                throw std::runtime_error("Unowned native entry changed. Restart SteamVR.");
        if (memory_.read(CameraPatch::Address, 8) != CameraPatch::Original)
            throw std::runtime_error("Unowned native camera predicate changed. Restart SteamVR.");
        return PatchResult::Restored;
    }
    if (action == TrackerPatch::Install) {
        if (!package_)
            throw std::logic_error("private tracker package is unavailable");
        package_->verify(process_.mappings());
    }
    return run_patch_worker(process_, [&](StoppedProcess &stopped) {
        TraceMemory memory(process_, stopped);
        if (action == TrackerPatch::CameraRestore) {
            {
                const auto state = TrackerControl(memory).state();
                if (state.command || state.manager || state.active || state.wrapper ||
                    state.classic)
                    throw std::runtime_error(
                        "Preserve native camera predicate until the private worker stops.");
            }
            CameraPatch(memory).restore();
            return PatchResult::Restored;
        }
        if (!package_)
            throw std::logic_error("private tracker package is unavailable");
        validate_adapter_mappings(process_.mappings(), package_->layout().loader.size());
        AdapterPatch patch(memory, package_->layout());
        if (action == TrackerPatch::RecoverUnarmed) {
            patch.recover_unarmed();
            return PatchResult::Restored;
        }
        if (action != TrackerPatch::Install)
            throw std::invalid_argument("unknown native tracker patch action");
        if (shared_call_active(stopped))
            return PatchResult::Busy;
        return patch.install() ? PatchResult::Applied : PatchResult::Reused;
    });
}
bool XrProcess::publish(const tracker::Control &state, uint64_t now) {
    if (!capability_)
        throw std::logic_error("private tracker readiness is unavailable");
    return capability_->publish(state, now);
}
void XrProcess::withdraw(uint64_t now) {
    if (capability_)
        capability_->withdraw(now);
}
} // namespace ct::helper
