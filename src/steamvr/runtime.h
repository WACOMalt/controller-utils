#pragma once
#include "queue_abi.h"
#include "transport.h"
#include "tracking/math.h"
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <openvr.h>

namespace ct {
namespace steamvr {
class QueueApi;
}
struct RuntimeDevice {
    uint32_t index;
    int device_class;
    bool valid;
    int tracking_result;
};
class RuntimeView {
  public:
    virtual ~RuntimeView() = default;
    virtual std::vector<RuntimeDevice> devices() = 0;
    virtual std::string serial(uint32_t index) = 0;
    virtual std::optional<RigidPose> head_pose(int origin = 1) = 0;
    virtual std::optional<TrackingSpace> tracking_space() {
        return {};
    }
    virtual std::optional<unsigned> primary_dashboard_device() {
        return std::nullopt;
    }
    virtual QueueContext &queues() = 0;
};
class Runtime final : public RuntimeView {
    void *library_ = nullptr;
    uint32_t (*initialize_)(vr::EVRInitError *, vr::EVRApplicationType) = nullptr;
    void (*shutdown_)() = nullptr;
    void *(*interface_)(const char *, vr::EVRInitError *) = nullptr;
    bool initialized_ = false;
    std::unique_ptr<steamvr::QueueApi> queue_api_;
    std::unique_ptr<QueueContext> queues_;

  public:
    using Device = RuntimeDevice;
    vr::IVRSystem *system = nullptr;
    vr::IVROverlay *overlay = nullptr;
    explicit Runtime(bool overlay = false,
                     const std::string &library = "/opt/steamvr/bin/linuxarm64/libopenvr_api.so");
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    ~Runtime() override;
    void *interface(const char *version) {
        if (!initialized_)
            throw std::logic_error("OpenVR runtime is closed");
        vr::EVRInitError error = vr::VRInitError_None;
        auto value = interface_(version, &error);
        if (!value || error)
            throw std::runtime_error(std::string(version) + ": " + std::to_string(error));
        return value;
    }
    std::vector<Device> devices() override;
    std::string serial(uint32_t index) override;
    std::optional<RigidPose> head_pose(int origin = 1) override;
    std::optional<TrackingSpace> tracking_space() override;
    std::optional<unsigned> primary_dashboard_device() override;
    QueueContext &queues() override;
    uint64_t path_handle(const char *name);
    void close();
};
void check_runtime(const std::string &directory = "/opt/steamvr");
} // namespace ct
