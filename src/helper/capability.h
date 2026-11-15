#pragma once
#include "platform/linux/protected_directory.h"
#include "tracking/native_abi.h"

namespace ct::helper {
class CapabilityPublisher {
    ProtectedDirectory directory_;
    uint32_t pid_;
    uint64_t started_;
    FileDescriptor existing(uint64_t now) const;

  public:
    CapabilityPublisher(ProtectedDirectory directory, pid_t pid, uint64_t started);
    bool publish(const tracker::Control &state, uint64_t now);
    void withdraw(uint64_t now);
};
} // namespace ct::helper
