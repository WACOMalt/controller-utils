#include "fake_openvr_interfaces.h"
#include "fake_openvr_overlay.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <array>
#include <unistd.h>
#include <openvr.h>

namespace {
unsigned init_count = 0, shutdown_count = 0, last_type = 0;
ct_test::System system_token;
ct_test::Applications applications_token;
ct_test::Overlay overlay_token;
} // namespace
extern "C" uint32_t VR_InitInternal(vr::EVRInitError *error, vr::EVRApplicationType application) {
    ++init_count;
    last_type = unsigned(application);
    *error =
        std::getenv("CT_TEST_OPENVR_INIT_FAILURE") ? vr::EVRInitError(100) : vr::VRInitError_None;
    return 1;
}
extern "C" void VR_ShutdownInternal() {
    ++shutdown_count;
}
extern "C" void *VR_GetGenericInterface(const char *name, vr::EVRInitError *error) {
    if (std::getenv("CT_TEST_OPENVR_INTERFACE_FAILURE")) {
        *error = vr::EVRInitError(105);
        return nullptr;
    }
    *error = vr::VRInitError_None;
    if (!std::strcmp(name, "IVRSystem_026"))
        return &system_token;
    if (!std::strcmp(name, "IVROverlay_028"))
        return &overlay_token;
    if (!std::strcmp(name, "IVRApplications_008"))
        return &applications_token;
    *error = vr::EVRInitError(105);
    return nullptr;
}
extern "C" void ct_test_reset() {
    init_count = shutdown_count = last_type = 0;
    ct_test::identified = ct_test::startup_calls = ct_test::pose_calls = 0;
    ct_test::startup_enabled = false;
    ct_test::keyboard_shown = ct_test::keyboard_positioned = 0;
    ct_test::next_overlay = ct_test::overlays_created = ct_test::overlays_destroyed =
        ct_test::textures_submitted = ct_test::overlays_shown = 0;
}
extern "C" unsigned ct_test_initialized() {
    return init_count;
}
extern "C" unsigned ct_test_shutdown() {
    return shutdown_count;
}
extern "C" unsigned ct_test_application_type() {
    return last_type;
}
extern "C" unsigned ct_test_identified() {
    return ct_test::identified;
}
extern "C" unsigned ct_test_startup_calls() {
    return ct_test::startup_calls;
}
extern "C" unsigned ct_test_startup_enabled() {
    return ct_test::startup_enabled;
}
extern "C" unsigned ct_test_pose_calls() {
    return ct_test::pose_calls;
}

extern "C" unsigned ct_test_overlays_created() {
    return ct_test::overlays_created;
}
extern "C" unsigned ct_test_overlays_destroyed() {
    return ct_test::overlays_destroyed;
}
extern "C" unsigned ct_test_textures_submitted() {
    return ct_test::textures_submitted;
}
extern "C" unsigned ct_test_overlays_shown() {
    return ct_test::overlays_shown;
}
extern "C" unsigned ct_test_keyboard_shown() {
    return ct_test::keyboard_shown;
}
extern "C" unsigned ct_test_keyboard_positioned() {
    return ct_test::keyboard_positioned;
}
