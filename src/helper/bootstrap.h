#pragma once
#include "patch.h"

namespace ct::helper {
class CameraPatch {
    PatchMemory &memory_;

  public:
    static constexpr uintptr_t Address = 0x267bf88;
    static const Bytes Original, Corrected;
    explicit CameraPatch(PatchMemory &memory) : memory_(memory) {}
    void apply();
    void restore();
};
} // namespace ct::helper
