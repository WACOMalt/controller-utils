#pragma once
#include "capability.h"
#include "package.h"
#include "tracker_control.h"
#include "worker.h"

namespace ct::helper {
enum class TrackerPatch { Install, RecoverUnarmed, CameraRestore, VerifyOriginal };
class BackendHost : public PatchMemory {
  public:
    virtual int descriptor() const = 0;
    virtual bool alive() const = 0;
    virtual void refresh() = 0;
    virtual PatchResult patch(TrackerPatch action) = 0;
    virtual bool publish(const tracker::Control &state, uint64_t now) = 0;
    virtual void withdraw(uint64_t now) = 0;
};
class XrProcess final : public BackendHost {
    PinnedProcess &process_;
    ProcessMemory memory_;
    NativePackage *package_;
    CapabilityPublisher *capability_;

  public:
    XrProcess(PinnedProcess &process, NativePackage *package, CapabilityPublisher *capability);
    int descriptor() const override {
        return process_.pid_descriptor();
    }
    bool alive() const override {
        return process_.alive();
    }
    void refresh() override;
    Bytes read(uintptr_t address, size_t size) override;
    void data(uintptr_t address, const Bytes &value) override;
    void code(uintptr_t address, const Bytes &value) override;
    PatchResult patch(TrackerPatch action) override;
    bool publish(const tracker::Control &state, uint64_t now) override;
    void withdraw(uint64_t now) override;
};
} // namespace ct::helper
