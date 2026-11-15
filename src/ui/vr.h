#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <time.h>
#include <openvr.h>
#include "interaction/pose_math.h"
#include "steamvr/runtime.h"

inline double native_time() {
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &time))
        throw std::runtime_error("cannot read Frame raw clock");
    return time.tv_sec + time.tv_nsec * 1e-9;
}

inline ct::Matrix matrix(const vr::HmdMatrix34_t &input) {
    auto output = ct::identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            output[c * 4 + r] = input.m[r][c];
    (void)ct::inverse_rigid(output);
    return output;
}

inline vr::HmdMatrix34_t matrix34(const ct::Matrix &input) {
    vr::HmdMatrix34_t output{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            output.m[r][c] = input[c * 4 + r];
    return output;
}

struct Vr {
    ct::Runtime *shared = nullptr;
    vr::IVRSystem *system = nullptr;
    vr::IVROverlay *overlay = nullptr;
    Vr() = default;
    explicit Vr(ct::Runtime &runtime)
        : shared(&runtime), system(runtime.system), overlay(runtime.overlay) {
        if (!system || !overlay)
            throw std::logic_error("Renderer requires the shared overlay runtime");
    }
    Vr(const Vr &) = delete;
    Vr &operator=(const Vr &) = delete;
    void *interface(const char *version) {
        if (!shared)
            throw std::logic_error("Renderer requires the shared runtime");
        return shared->interface(version);
    }
    std::optional<ct::Matrix> head() {
        vr::TrackedDevicePose_t pose{};
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, .011f, &pose, 1);
        if (!pose.bDeviceIsConnected || !pose.bPoseIsValid ||
            pose.eTrackingResult != vr::TrackingResult_Running_OK)
            return {};
        return matrix(pose.mDeviceToAbsoluteTracking);
    }
};

inline void check(vr::EVROverlayError error, const char *operation) {
    if (error)
        throw std::runtime_error(std::string("body overlay ") + operation + ": " +
                                 std::to_string(error));
}
