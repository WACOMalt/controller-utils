#pragma once
#include "patch.h"
#include "platform/json.h"
#include "tracking/acquisition.h"
#include <functional>
#include <cstring>

namespace ct::helper {
using MemoryRead = std::function<Bytes(uintptr_t, size_t)>;
class AcquisitionReader {
    MemoryRead read_;
    std::function<double()> raw_time_;
    Bytes bytes(uintptr_t address, size_t size);
    template <class T> T field(uintptr_t address) {
        const auto value = bytes(address, sizeof(T));
        T result;
        std::memcpy(&result, value.data(), sizeof(result));
        return result;
    }
    uintptr_t pointer(uintptr_t address);

  public:
    static constexpr uintptr_t AppGlobal = 0x3244ef8, AppVtable = 0x316b890;
    explicit AcquisitionReader(MemoryRead read, std::function<double()> raw_time = {});
    json::Value record(uintptr_t address);
    json::Value feature_group(uintptr_t address);
    json::Value palm_estimator(uintptr_t address, bool include_pose = false);
    json::Value snapshot(std::optional<uintptr_t> wrapper_override = {}, bool hands_only = false);
};
AcquisitionQuality acquisition_quality(json_object *state);
} // namespace ct::helper
