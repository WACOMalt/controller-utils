#pragma once
#include "bootstrap.h"
#include "platform/json.h"

namespace ct::helper {
struct AdapterSite {
    uintptr_t address;
    Bytes original, redirect;
};
struct AdapterLayout {
    std::vector<AdapterSite> sites;
    Bytes loader, cave_original;
    static AdapterLayout from_json(json_object *manifest, const Bytes &loader);
};
class AdapterPatch {
    PatchMemory &memory_;
    AdapterLayout layout_;
    void verify_unarmed();
    void rollback_install();

  public:
    static constexpr uintptr_t Control = 0x343ff60, Cave = 0x315abb0, Camera = CameraPatch::Address;
    static constexpr uint64_t Magic = 0x314c4f5349544343ULL;
    AdapterPatch(PatchMemory &memory, AdapterLayout layout);
    bool install();
    void recover_unarmed();
    void restore_camera();
};
bool shared_call_active(StoppedProcess &process);
void validate_adapter_mappings(const std::vector<MemoryMapping> &mappings, size_t loader_size);
} // namespace ct::helper
