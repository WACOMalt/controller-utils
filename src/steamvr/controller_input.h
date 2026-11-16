#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <openvr.h>

namespace ct::steamvr {
struct GripState {
    uint64_t valid = 0, down = 0;
};

struct ControllerInput {
    struct PathRead {
        uint64_t path;
        void *buffer;
        uint32_t size, tag, required;
        int error;
        const char *name;
    };
    // Tag 33 contains a typed input value, including the driver's last update time.
    struct InputValue {
        uint64_t path, time;
        uint32_t type, visibility;
        std::array<uint8_t, 48> value;
    };
    static_assert(sizeof(PathRead) == 40 && sizeof(InputValue) == 72);
    int (*read_path)(uint64_t, PathRead *, uint32_t) = nullptr;
    uint64_t (*container)(uint32_t) = nullptr;

    GripState grips(vr::IVRSystem &system) const {
        static_assert(vr::k_unMaxTrackedDeviceCount <= 64);
        GripState result;
        if (!read_path || !container)
            return result;
        for (uint32_t device = 0; device < vr::k_unMaxTrackedDeviceCount; ++device) {
            if (!system.IsTrackedDeviceConnected(device) ||
                system.GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller)
                continue;
            char type[128]{};
            vr::ETrackedPropertyError error = vr::TrackedProp_Success;
            const auto size = system.GetStringTrackedDeviceProperty(
                device, vr::Prop_ControllerType_String, type, sizeof(type), &error);
            if (error || size != sizeof("frame_controller") ||
                std::memcmp(type, "frame_controller", sizeof("frame_controller")))
                continue;
            const auto root = container(device);
            if (!root)
                continue;
            InputValue value{};
            PathRead read{0, &value, sizeof(value), 0, 0, 0, "/input/grip/click"};
            if (read_path(root, &read, 1) || read.error || read.tag != 33 ||
                read.required != sizeof(value) || value.type != 1 || value.visibility != 3 ||
                value.value[0] > 1)
                continue;
            const auto bit = uint64_t(1) << device;
            result.valid |= bit;
            if (value.value[0])
                result.down |= bit;
        }
        return result;
    }
};
} // namespace ct::steamvr
