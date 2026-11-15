#pragma once
#include "adapter_patch.h"
#include "platform/json.h"
#include "tracking/native_abi.h"
#include <cstring>

namespace ct::helper {
class TrackerControl {
    PatchMemory &memory_;
    std::optional<uint32_t> generation_;
    template <class T> void write(size_t offset, const T &value) {
        Bytes bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        memory_.data(AdapterPatch::Control + offset, bytes);
        if (memory_.read(AdapterPatch::Control + offset, sizeof(value)) != bytes)
            throw std::runtime_error("Native adapter control write failed.");
    }

  public:
    explicit TrackerControl(PatchMemory &memory) : memory_(memory) {}
    tracker::Control state();
    uint32_t begin(uint64_t raw_ns);
    void renew(uint64_t raw_ns);
    void request_stop();
    bool stopped();
    std::optional<uint32_t> generation() const {
        return generation_;
    }
};
json::Value tracker_status(const tracker::Control &state);
} // namespace ct::helper
