#pragma once
#include "trace.h"
#include <functional>
#include <string>
#include <vector>

namespace ct::helper {
using Bytes = std::vector<uint8_t>;
class PatchMemory {
  public:
    virtual ~PatchMemory() = default;
    virtual Bytes read(uintptr_t address, size_t size) = 0;
    virtual void code(uintptr_t address, const Bytes &value) = 0;
    virtual void data(uintptr_t address, const Bytes &value) = 0;
};
class TraceMemory final : public PatchMemory {
    PinnedProcess &process_;
    StoppedProcess &stopped_;

  public:
    TraceMemory(PinnedProcess &process, StoppedProcess &stopped)
        : process_(process), stopped_(stopped) {}
    Bytes read(uintptr_t address, size_t size) override;
    void code(uintptr_t address, const Bytes &value) override;
    void data(uintptr_t address, const Bytes &value) override;
};
class ProcessMemory final : public PatchMemory {
    PinnedProcess &process_;

  public:
    explicit ProcessMemory(PinnedProcess &process) : process_(process) {}
    Bytes read(uintptr_t address, size_t size) override;
    void code(uintptr_t address, const Bytes &value) override;
    void data(uintptr_t address, const Bytes &value) override;
};
enum class PatchStorage { Code, Data };
void patch_words(PatchMemory &memory, PatchStorage storage, uintptr_t address, const Bytes &value);
enum class GateKind { Input, Beam };
struct GateLayout {
    GateKind kind;
    uintptr_t site, cave;
    Bytes original, following, redirect, payload;
    size_t index_offset;
    uint32_t index_limit;
};
GateLayout place_gate(GateKind kind, uintptr_t site, uintptr_t cave);
GateLayout resolve_gate_layout(GateKind kind, const std::string &image);
struct BeamContract {
    size_t before, size;
    const char *signature, *context;
};
GateLayout resolve_beam_reviewed(const std::string &image,
                                 const std::vector<BeamContract> &contracts);
void validate_gate_mappings(const GateLayout &layout, uintptr_t base,
                            const std::vector<MemoryMapping> &mappings);
std::optional<uintptr_t> gate_resume_pc(const GateLayout &layout, uintptr_t base, uintptr_t pc);
bool owned_gate(const GateLayout &layout, const Bytes &value);
void validate_gate_start(const GateLayout &layout, const Bytes &site, const Bytes &cave,
                         bool restoring);
uint32_t validate_live_gate(const GateLayout &layout, const Bytes &site, const Bytes &cave);
class GatePatch {
    PatchMemory &memory_;
    GateLayout layout_;
    uintptr_t base_;
    void context();

  public:
    GatePatch(PatchMemory &memory, const GateLayout &layout, uintptr_t base);
    const GateLayout &layout() const {
        return layout_;
    }
    void apply();
    void restore(const std::function<void()> &evacuate);
};
uint32_t aarch64_branch(uintptr_t source, uintptr_t destination);
} // namespace ct::helper
