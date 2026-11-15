#include "gate.h"
#include "platform/linux/error.h"
#include "steamvr/compatibility.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace ct::helper {
namespace {
uint32_t value(const Bytes &bytes) {
    if (bytes.size() != 4)
        throw std::invalid_argument("invalid native gate literal");
    uint32_t result;
    std::memcpy(&result, bytes.data(), 4);
    return result;
}
void time_value(double now) {
    if (!std::isfinite(now) || now < 0)
        throw std::invalid_argument("invalid native gate clock");
}
} // namespace
GateMemory::GateMemory(PinnedProcess &process, const GateLayout &layout)
    : process_(process), layout_(layout), base_(process.image_base()) {
    const auto &binary = layout.kind == GateKind::Input ? steamvr::compatibility::SteamWebHelper
                                                        : steamvr::compatibility::Compositor;
    if (!process_.writable_ || process_.spec_.owner != 1000 ||
        process_.spec_.executable != binary.path)
        throw std::invalid_argument("unsupported native gate process");
    process_.refresh();
    validate_gate_mappings(layout_, base_, process_.mappings());
}
Bytes GateMemory::read(uintptr_t address, size_t size) {
    return process_.read(address, size);
}
void GateMemory::code(uintptr_t, const Bytes &) {
    throw std::logic_error("live gate cannot write instructions");
}
void GateMemory::data(uintptr_t address, const Bytes &bytes) {
    if (address != base_ + layout_.cave + layout_.index_offset ||
        (value(bytes) != UINT32_MAX && value(bytes) >= layout_.index_limit))
        throw std::invalid_argument("live gate write is outside the approved literal");
    process_.refresh();
    validate_gate_mappings(layout_, base_, process_.mappings());
    validate_live_gate(layout_, read(base_ + layout_.site, 12),
                       read(base_ + layout_.cave, layout_.payload.size()));
    ssize_t count;
    do {
        count = pwrite(process_.memory_.get(), bytes.data(), 4, off_t(address));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        throw std::system_error(errno, std::generic_category(), "write native gate literal");
    if (count != 4 || read(address, 4) != bytes)
        throw ConnectionError("Steam UI input gate index write failed.");
}
GateLease::GateLease(PatchMemory &memory, const GateLayout &layout, uintptr_t base)
    : memory_(memory), layout_(layout), base_(base) {
    if (base % 4096 ||
        base > UINTPTR_MAX - std::max(layout_.site + 12, layout_.cave + layout_.payload.size()))
        throw std::invalid_argument("invalid native gate base");
    const auto current =
        validate_live_gate(layout_, memory_.read(base_ + layout_.site, 12),
                           memory_.read(base_ + layout_.cave, layout_.payload.size()));
    if (current != UINT32_MAX)
        throw std::runtime_error("Steam UI input gate did not start disabled.");
}
void GateLease::set(uint32_t index) {
    const auto current =
        validate_live_gate(layout_, memory_.read(base_ + layout_.site, 12),
                           memory_.read(base_ + layout_.cave, layout_.payload.size()));
    if (current != index) {
        Bytes bytes(4);
        std::memcpy(bytes.data(), &index, 4);
        memory_.data(base_ + layout_.cave + layout_.index_offset, bytes);
        if (memory_.read(base_ + layout_.cave + layout_.index_offset, 4) != bytes)
            throw ConnectionError("Steam UI input gate index write failed.");
    }
    index_ = index;
}
void GateLease::renew(uint32_t index, double now) {
    time_value(now);
    if (index >= layout_.index_limit)
        throw std::invalid_argument("Invalid Steam UI controller index.");
    set(index);
    expires_ = now + Lifetime;
}
void GateLease::disable() {
    set(UINT32_MAX);
    expires_.reset();
}
void GateLease::poll(double now) {
    time_value(now);
    if (expires_ && now >= *expires_)
        disable();
}
} // namespace ct::helper
