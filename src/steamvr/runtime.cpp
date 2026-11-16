#include "runtime.h"
#include "compatibility.h"
#include "queue_api.h"
#include "platform/linux/files.h"
#include <array>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace ct {
Runtime::Runtime(bool use_overlay, const std::string &library) {
    library_ = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library_)
        throw std::runtime_error("Frame's OpenVR library is unavailable");
    try {
        initialize_ = reinterpret_cast<decltype(initialize_)>(dlsym(library_, "VR_InitInternal"));
        shutdown_ = reinterpret_cast<decltype(shutdown_)>(dlsym(library_, "VR_ShutdownInternal"));
        interface_ =
            reinterpret_cast<decltype(interface_)>(dlsym(library_, "VR_GetGenericInterface"));
        if (!initialize_ || !shutdown_ || !interface_)
            throw std::runtime_error("missing OpenVR exports");
        vr::EVRInitError error = vr::VRInitError_None;
        initialize_(&error, use_overlay ? vr::VRApplication_Overlay : vr::VRApplication_Background);
        if (error)
            throw std::runtime_error("OpenVR background init: " + std::to_string(error));
        initialized_ = true;
        system = static_cast<vr::IVRSystem *>(interface(vr::IVRSystem_Version));
        if (use_overlay)
            overlay = static_cast<vr::IVROverlay *>(interface(vr::IVROverlay_Version));
    } catch (...) {
        if (initialized_)
            shutdown_();
        initialized_ = false;
        dlclose(library_);
        library_ = nullptr;
        throw;
    }
}
Runtime::~Runtime() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
}
std::vector<Runtime::Device> Runtime::devices() {
    if (!initialized_)
        throw std::logic_error("OpenVR runtime is closed");
    std::array<vr::TrackedDevicePose_t, 64> poses{};
    system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses.data(),
                                            poses.size());
    std::vector<Device> result;
    for (uint32_t index = 0; index < poses.size(); ++index) {
        const auto &pose = poses[index];
        if (pose.bDeviceIsConnected)
            result.push_back({index, int(system->GetTrackedDeviceClass(index)), pose.bPoseIsValid,
                              int(pose.eTrackingResult)});
    }
    return result;
}
std::string Runtime::serial(uint32_t index) {
    if (!initialized_)
        throw std::logic_error("OpenVR runtime is closed");
    std::array<char, 256> buffer{};
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    const auto size = system->GetStringTrackedDeviceProperty(index, vr::Prop_SerialNumber_String,
                                                             buffer.data(), buffer.size(), &error);
    if (error || size <= 1 || size > buffer.size() || !std::memchr(buffer.data(), 0, size))
        throw std::runtime_error("cannot read tracked-device serial");
    const std::string result(buffer.data());
    for (unsigned char c : result)
        if (c > 127)
            throw std::runtime_error("cannot read tracked-device serial");
    return result;
}
std::optional<RigidPose> Runtime::head_pose(int origin) {
    if (origin < 0 || origin > 2)
        throw std::invalid_argument("invalid tracking origin");
    if (!initialized_)
        throw std::logic_error("OpenVR runtime is closed");
    vr::TrackedDevicePose_t pose{};
    system->GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin(origin), 0, &pose, 1);
    if (!pose.bDeviceIsConnected || !pose.bPoseIsValid ||
        pose.eTrackingResult != vr::TrackingResult_Running_OK)
        return {};
    RigidPose result{};
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 4; ++j)
            result[i * 4 + j] = pose.mDeviceToAbsoluteTracking.m[i][j];
    return result;
}
std::optional<unsigned> Runtime::primary_dashboard_device() {
    if (!initialized_)
        throw std::logic_error("OpenVR runtime is closed");
    if (overlay) {
        const auto device = overlay->GetPrimaryDashboardDevice();
        if (device < vr::k_unMaxTrackedDeviceCount && system->IsTrackedDeviceConnected(device))
            return device;
    }
    return std::nullopt;
}
QueueContext &Runtime::queues() {
    if (!initialized_)
        throw std::logic_error("OpenVR runtime is closed");
    if (!queues_) {
        const auto table = interface("FnTable:IVRBlockQueue_005");
        const auto module = steamvr::Module::loaded(table, steamvr::compatibility::VrClient.path);
        auto queue = std::make_unique<steamvr::QueueApi>(table, module);
        auto context = std::make_unique<QueueContext>(*queue);
        queue_api_ = std::move(queue);
        queues_ = std::move(context);
    }
    return *queues_;
}
uint64_t Runtime::path_handle(const char *name) {
    if (!name || !*name)
        throw std::invalid_argument("OpenVR path is empty");
    const auto table = interface("FnTable:IVRPaths_002");
    const auto module = steamvr::Module::loaded(table, steamvr::compatibility::VrClient.path);
    const auto functions = steamvr::read_functions<4>(table, module);
    const auto convert = reinterpret_cast<int (*)(uint64_t *, char *)>(functions[2]);
    uint64_t handle = 0;
    if (convert(&handle, const_cast<char *>(name)) || !handle)
        throw std::runtime_error("SteamVR path is unavailable");
    return handle;
}
void Runtime::close() {
    std::exception_ptr failure;
    if (queues_) {
        try {
            queues_->close();
        } catch (...) {
            failure = std::current_exception();
        }
        queues_.reset();
    }
    queue_api_.reset();
    if (initialized_) {
        initialized_ = false;
        shutdown_();
    }
    system = nullptr;
    overlay = nullptr;
    if (library_) {
        dlclose(library_);
        library_ = nullptr;
    }
    if (failure)
        std::rethrow_exception(failure);
}
void check_runtime(const std::string &directory) {
    const auto &binary = steamvr::compatibility::XrService;
    if (!steamvr::compatibility::supported_xr_service(
            sha256(std::filesystem::path(directory) /
                   std::filesystem::path(binary.path).lexically_relative("/opt/steamvr"))))
        throw std::runtime_error(std::string(binary.name) +
                                 " changed. Native protocol must be reverified");
}
} // namespace ct
