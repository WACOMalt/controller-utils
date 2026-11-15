#pragma once
#include "patch.h"

namespace ct::helper {
class GateMemory final : public PatchMemory {
    PinnedProcess &process_;
    GateLayout layout_;
    uintptr_t base_;

  public:
    GateMemory(PinnedProcess &process, const GateLayout &layout);
    Bytes read(uintptr_t address, size_t size) override;
    void code(uintptr_t address, const Bytes &value) override;
    void data(uintptr_t address, const Bytes &value) override;
};
class GateLease {
    PatchMemory &memory_;
    GateLayout layout_;
    uintptr_t base_;
    uint32_t index_ = UINT32_MAX;
    std::optional<double> expires_;
    void set(uint32_t index);

  public:
    static constexpr double Lifetime = .75;
    GateLease(PatchMemory &memory, const GateLayout &layout, uintptr_t base);
    uint32_t index() const {
        return index_;
    }
    std::optional<double> expires() const {
        return expires_;
    }
    void renew(uint32_t index, double now);
    void disable();
    void poll(double now);
};
} // namespace ct::helper
