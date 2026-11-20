#pragma once
#include "platform/linux/process.h"
#include <string>
#include <vector>

namespace ct::steamvr {
using Bytes = std::vector<uint8_t>;
enum class GateKind { Input, Beam };
struct GateLayout {
    GateKind kind;
    uintptr_t site, cave;
    Bytes original, following, redirect, payload;
    size_t index_offset;
    uint32_t index_limit;
};
uint32_t aarch64_branch(uintptr_t source, uintptr_t destination);
GateLayout place_gate(GateKind kind, uintptr_t site, uintptr_t cave, unsigned beam_register = 11);
GateLayout resolve_gate_layout(GateKind kind, const std::string &image);
struct BeamContract {
    size_t before, size;
    const char *signature, *context;
    unsigned beam_register = 11;
};
GateLayout resolve_beam_reviewed(const std::string &image,
                                 const std::vector<BeamContract> &contracts);
void validate_gate_mappings(const GateLayout &layout, uintptr_t base,
                            const std::vector<MemoryMapping> &mappings);
} // namespace ct::steamvr
