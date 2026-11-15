#pragma once
#include "../driver/tracked_input.h"
#include <cassert>
#include <cstring>
#include <map>
#include <vector>

struct Properties final : vr::IVRProperties {
    struct Value {
        std::vector<unsigned char> data;
        vr::PropertyTypeTag_t tag;
    };
    std::map<std::pair<uint64_t, int>, Value> values;
    vr::ETrackedPropertyError ReadPropertyBatch(uint64_t container, vr::PropertyRead_t *batch,
                                                uint32_t count) override {
        for (uint32_t i = 0; i < count; ++i) {
            auto &item = batch[i];
            auto found = values.find({container, item.prop});
            item.eError = vr::TrackedProp_UnknownProperty;
            item.unTag = 0;
            item.unRequiredBufferSize = 0;
            if (found == values.end())
                continue;
            auto &value = found->second;
            item.unRequiredBufferSize = uint32_t(value.data.size());
            item.unTag = value.tag;
            item.eError = item.unBufferSize < value.data.size() ? vr::TrackedProp_BufferTooSmall
                                                                : vr::TrackedProp_Success;
            if (item.eError == vr::TrackedProp_Success)
                std::memcpy(item.pvBuffer, value.data.data(), value.data.size());
        }
        return vr::TrackedProp_Success;
    }
    vr::ETrackedPropertyError WritePropertyBatch(uint64_t container, vr::PropertyWrite_t *batch,
                                                 uint32_t count) override {
        for (uint32_t i = 0; i < count; ++i) {
            auto &item = batch[i];
            const auto *bytes = static_cast<unsigned char *>(item.pvBuffer);
            values[{container, item.prop}] = {{bytes, bytes + item.unBufferSize}, item.unTag};
            item.eError = vr::TrackedProp_Success;
        }
        return vr::TrackedProp_Success;
    }
    const char *GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override {
        return "test";
    }
    uint64_t TrackedDeviceToPropertyContainer(uint32_t index) override {
        return 100 + index;
    }
    int role(uint32_t index = 10) {
        return vr::CVRPropertyHelpers(this).GetInt32Property(100 + index,
                                                             vr::Prop_ControllerRoleHint_Int32);
    }
    void controller(uint32_t index, int role) {
        vr::CVRPropertyHelpers helper(this);
        helper.SetInt32Property(100 + index, vr::Prop_DeviceClass_Int32,
                                vr::TrackedDeviceClass_Controller);
        helper.SetInt32Property(100 + index, vr::Prop_ControllerRoleHint_Int32, role);
    }
};

struct Input final : vr::IVRDriverInput {
    std::map<uint64_t, std::string> paths;
    std::map<std::string, double> values;
    std::vector<std::string> updates;
    vr::HmdMatrix34_t tip{};
    bool fail_once = false, fail_pose_once = false;
    vr::EVRInputError create(const char *name, uint64_t *handle) {
        *handle = paths.size() + 1;
        paths[*handle] = name;
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreateBooleanComponent(uint64_t, const char *name,
                                             uint64_t *handle) override {
        return create(name, handle);
    }
    vr::EVRInputError CreateScalarComponent(uint64_t, const char *name, uint64_t *handle,
                                            vr::EVRScalarType type,
                                            vr::EVRScalarUnits units) override {
        assert(type == vr::VRScalarType_Absolute);
        assert(units == ((std::strstr(name, "thumbstick") || std::strstr(name, "pad/"))
                             ? vr::VRScalarUnits_NormalizedTwoSided
                             : vr::VRScalarUnits_NormalizedOneSided));
        return create(name, handle);
    }
    vr::EVRInputError set(uint64_t handle, double value) {
        updates.push_back(paths.at(handle));
        if (fail_once) {
            fail_once = false;
            return vr::VRInputError_InvalidHandle;
        }
        values[paths.at(handle)] = value;
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateBooleanComponent(uint64_t handle, bool value, double) override {
        return set(handle, value);
    }
    vr::EVRInputError UpdateScalarComponent(uint64_t handle, float value, double) override {
        return set(handle, value);
    }
    vr::EVRInputError CreatePoseComponent(uint64_t, const char *name, uint64_t *handle) override {
        return create(name, handle);
    }
    vr::EVRInputError UpdatePoseComponent(uint64_t, const vr::HmdMatrix34_t *value,
                                          double) override {
        if (fail_pose_once) {
            fail_pose_once = false;
            return vr::VRInputError_InvalidHandle;
        }
        tip = *value;
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreateHapticComponent(uint64_t, const char *, uint64_t *) override {
        assert(false);
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreateSkeletonComponent(uint64_t, const char *, const char *, const char *,
                                              vr::EVRSkeletalTrackingLevel,
                                              const vr::VRBoneTransform_t *, uint32_t,
                                              uint64_t *) override {
        assert(false);
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateSkeletonComponent(uint64_t, vr::EVRSkeletalMotionRange,
                                              const vr::VRBoneTransform_t *, uint32_t) override {
        assert(false);
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreateEyeTrackingComponent(uint64_t, const char *, uint64_t *) override {
        assert(false);
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateEyeTrackingComponent(uint64_t, const vr::VREyeTrackingData_t *,
                                                 double) override {
        assert(false);
        return vr::VRInputError_None;
    }
    bool released() const {
        for (auto &value : values)
            if (value.second != 0)
                return false;
        return true;
    }
};

struct Host final : vr::IVRServerDriverHost {
    std::string serial;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    vr::DriverPose_t published{};
    int adds = 0, publishes = 0;
    bool TrackedDeviceAdded(const char *value, vr::ETrackedDeviceClass type,
                            vr::ITrackedDeviceServerDriver *device) override {
        ++adds;
        serial = value;
        assert(type == vr::TrackedDeviceClass_Controller);
        return device->Activate(10) == vr::VRInitError_None;
    }
    void TrackedDevicePoseUpdated(uint32_t index, const vr::DriverPose_t &pose,
                                  uint32_t size) override {
        assert(index == 10 && size == sizeof(pose));
        published = pose;
        ++publishes;
        poses[index].bDeviceIsConnected = pose.deviceIsConnected;
    }
    void GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t *output, uint32_t count) override {
        assert(count == vr::k_unMaxTrackedDeviceCount);
        std::copy(poses, poses + count, output);
    }
    void VsyncEvent(double) override {
        assert(false);
    }
    void VendorSpecificEvent(uint32_t, vr::EVREventType, const vr::VREvent_Data_t &,
                             double) override {
        assert(false);
    }
    bool IsExiting() override {
        return false;
    }
    bool PollNextEvent(vr::VREvent_t *, uint32_t) override {
        return false;
    }
    void RequestRestart(const char *, const char *, const char *, const char *) override {
        assert(false);
    }
    uint32_t GetFrameTimings(vr::Compositor_FrameTiming *, uint32_t) override {
        assert(false);
        return 0;
    }
    void SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t &,
                             const vr::HmdMatrix34_t &) override {
        assert(false);
    }
    void SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t &,
                                 const vr::HmdRect2_t &) override {
        assert(false);
    }
    void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {
        assert(false);
    }
};
