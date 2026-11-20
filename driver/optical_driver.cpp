#include "publish_input.h"
#include "steamvr/optical_socket.h"
#include "platform/linux/resource.h"
#include <memory>
#include <cerrno>
#include <ctime>

namespace {
double now() {
    timespec t{};
    return clock_gettime(CLOCK_MONOTONIC_RAW, &t) == 0 ? t.tv_sec + t.tv_nsec * 1e-9 : 0;
}
class Provider final : public vr::IServerTrackedDeviceProvider {
    ct::FileDescriptor listener, peer;
    std::unique_ptr<ct::TrackedInput> tracked;
    ct::wire::AppInput input{};
    double last = 0;
    void release() {
        if (tracked)
            tracked->yield();
        peer.reset();
        input = {};
        last = 0;
    }

  public:
    vr::EVRInitError Init(vr::IVRDriverContext *context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        try {
            tracked = std::make_unique<ct::TrackedInput>(
                vr::VRServerDriverHost(), vr::VRDriverInput(), vr::VRPropertiesRaw());
            listener.reset(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
            sockaddr_un addr{};
            const auto size = ct::optical::address(addr);
            if (!listener || bind(listener.get(), reinterpret_cast<sockaddr *>(&addr), size) ||
                listen(listener.get(), 1))
                throw std::runtime_error("Cannot open the Controller Utils optical socket");
        } catch (const std::exception &e) {
            vr::VRDriverLog()->Log(e.what());
            Cleanup();
            return vr::VRInitError_Driver_Failed;
        }
        return vr::VRInitError_None;
    }
    void Cleanup() override {
        release();
        listener.reset();
        tracked.reset();
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    const char *const *GetInterfaceVersions() override {
        return vr::k_InterfaceVersions;
    }
    void RunFrame() override {
        if (!listener)
            return;
        if (!peer) {
            peer.reset(accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!peer)
                return;
            if (!ct::optical::same_user(peer.get())) {
                release();
                return;
            }
        }
        const auto t = now();
        bool updated = false;
        for (int i = 0; i < 32; ++i) {
            ct::optical::Packet p{};
            const auto n = recv(peer.get(), &p, sizeof(p), MSG_TRUNC | MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (n != sizeof(p) || p.version != 1 || !memchr(p.serial, 0, sizeof(p.serial))) {
                release();
                return;
            }
            if (!std::isfinite(p.input.session_start) || p.input.session_start <= 0 ||
                !ct::wire::app_input(p.input, t, last,
                                     last ? input.session_start : p.input.session_start))
                continue;
            try {
                if (!tracked->register_device(p.serial)) {
                    release();
                    return;
                }
            } catch (const std::exception &) {
                release();
                return;
            }
            input = p.input;
            last = input.time;
            ct::publish_input(*tracked, input, t);
            updated = true;
        }
        if (updated)
            return;
        if (last && t - last <= .15)
            ct::publish_input(*tracked, input, t);
        else
            tracked->yield();
    }
    bool ShouldBlockStandbyMode() override {
        return false;
    }
    void EnterStandby() override {
        release();
    }
    void LeaveStandby() override {}
};
Provider provider;
} // namespace
extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name,
                                                                         int *code) {
    if (name && !strcmp(name, vr::IServerTrackedDeviceProvider_Version))
        return &provider;
    if (code)
        *code = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
