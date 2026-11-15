#include "patch.h"
#include "steamvr/compatibility.h"
#include "steamvr/code.h"
#include "steamvr/image.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <exception>
#include <limits>
#include <stdexcept>

namespace ct::helper {
namespace {
Bytes hex(const std::string &text) {
    if (text.size() % 2)
        throw std::logic_error("invalid compiled patch bytes");
    Bytes out;
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return unsigned(c - '0');
        if (c >= 'a' && c <= 'f')
            return unsigned(c - 'a' + 10);
        throw std::logic_error("invalid compiled patch bytes");
    };
    for (size_t i = 0; i < text.size(); i += 2)
        out.push_back(uint8_t(digit(text[i]) * 16 + digit(text[i + 1])));
    return out;
}
uint32_t u32(const Bytes &value, size_t offset) {
    if (offset > value.size() || value.size() - offset < 4)
        throw std::invalid_argument("truncated patch word");
    uint32_t result;
    std::memcpy(&result, value.data() + offset, 4);
    return result;
}
Bytes word(uint32_t value) {
    Bytes bytes(4);
    std::memcpy(bytes.data(), &value, 4);
    return bytes;
}
} // namespace
void patch_words(PatchMemory &memory, PatchStorage storage, uintptr_t address, const Bytes &value) {
    if (address % 4 || value.empty() || value.size() % 4 || value.size() > 4096)
        throw std::invalid_argument("unaligned or oversized patch range");
    for (size_t offset = 0; offset < value.size(); offset += 4) {
        const Bytes part(value.begin() + offset, value.begin() + offset + 4);
        if (storage == PatchStorage::Code)
            memory.code(address + offset, part);
        else
            memory.data(address + offset, part);
        if (memory.read(address + offset, 4) != part)
            throw std::runtime_error("instruction write did not read back");
    }
}
Bytes TraceMemory::read(uintptr_t address, size_t size) {
    return stopped_.read(address, size);
}
void TraceMemory::code(uintptr_t address, const Bytes &value) {
    stopped_.write_code(address, value);
}
void TraceMemory::data(uintptr_t address, const Bytes &value) {
    stopped_.check();
    process_.write_data(address, value);
}
Bytes ProcessMemory::read(uintptr_t address, size_t size) {
    return process_.read(address, size);
}
void ProcessMemory::code(uintptr_t, const Bytes &) {
    throw std::logic_error("live process cannot write instructions");
}
void ProcessMemory::data(uintptr_t address, const Bytes &value) {
    process_.write_data(address, value);
}
uint32_t aarch64_branch(uintptr_t source, uintptr_t destination) {
    if (source % 4 || destination % 4 ||
        (destination >= source ? destination - source >= (1u << 27)
                               : source - destination > (1u << 27)))
        throw std::invalid_argument("AArch64 branch target out of range");
    const int64_t delta =
        destination >= source ? int64_t(destination - source) : -int64_t(source - destination);
    return 0x14000000u | (uint32_t(delta / 4) & 0x3ffffffu);
}
GateLayout place_gate(GateKind kind, uintptr_t site, uintptr_t cave) {
    if (site > UINTPTR_MAX - 12 || cave > UINTPTR_MAX - 60)
        throw std::invalid_argument("native gate address overflow");
    GateLayout layout;
    if (kind == GateKind::Beam)
        layout = {
            kind,
            site,
            cave,
            hex("8b1244bd"),
            hex("9609211e250a211e"),
            {},
            hex("f047bea910423bd5f00b00f98b1244bdf05f41b931010010310240b91f02116b41000054eb0327"
                "1ef00b40f910421bd5f047c2a84c3aec17ffffffff"),
            56,
            64};
    else if (kind == GateKind::Input)
        layout = {kind,
                  site,
                  cave,
                  hex("219040f9"),
                  hex("20003fd6772e40f9"),
                  {},
                  hex("219040f9b00640b9b10000181f02116b400000541bbf05141bbf0514ffffffff"),
                  28,
                  16};
    else
        throw std::invalid_argument("unknown native gate kind");
    layout.redirect = word(aarch64_branch(site, cave));
    auto relocate = [&](size_t offset, uintptr_t target) {
        const auto branch = aarch64_branch(cave + offset, site + target);
        std::memcpy(layout.payload.data() + offset, &branch, 4);
    };
    if (kind == GateKind::Input) {
        relocate(20, 4);
        relocate(24, 8);
    } else
        relocate(52, 4);
    return layout;
}
GateLayout resolve_gate_reviewed(GateKind kind, const std::string &bytes, const char *expected) {
    const auto prototype = place_gate(kind, 0, 0);
    auto pattern = prototype.original;
    pattern.insert(pattern.end(), prototype.following.begin(), prototype.following.end());
    steamvr::Image image(bytes);
    const auto site = image.unique(pattern);
    const uintptr_t before = kind == GateKind::Input ? 44 : 64;
    if (site < before)
        throw std::runtime_error("native gate context is outside the image");
    if (!expected)
        throw std::invalid_argument("native gate signature is unavailable");
    if (steamvr::code_signature(image.at(site - before, 128, true), 128) != expected)
        throw std::runtime_error("native gate context signature mismatch");
    const auto cave = image.padding(kind == GateKind::Input, prototype.payload.size());
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        throw std::runtime_error("native page size is unavailable");
    const auto data = kind == GateKind::Input
                          ? image.at(cave, prototype.payload.size(), true)
                          : image.mapped_tail(cave, prototype.payload.size(), size_t(page_size));
    if (!std::all_of(data, data + prototype.payload.size(), [](char byte) { return !byte; }))
        throw std::runtime_error("native gate padding is occupied");
    return place_gate(kind, site, cave);
}
GateLayout resolve_beam_reviewed(const std::string &bytes,
                                 const std::vector<BeamContract> &contracts) {
    steamvr::Image image(bytes);
    auto pattern = place_gate(GateKind::Beam, 0, 0).original;
    const auto following = place_gate(GateKind::Beam, 0, 0).following;
    pattern.insert(pattern.end(), following.begin(), following.end());
    const auto site = image.unique(pattern);
    std::optional<GateLayout> result;
    for (const auto &contract : contracts) {
        if (site < contract.before || !contract.signature || contract.size < 0x70 ||
            contract.size > 16384 || contract.size % 4 || contract.before > contract.size - 12)
            continue;
        const auto body = image.at(site - contract.before, contract.size, true);
        if (steamvr::code_signature(body, contract.size) != contract.signature)
            continue;
        if (result)
            throw std::runtime_error("ambiguous native beam contract");
        uint32_t frame, initialize, store;
        std::memcpy(&frame, body, 4);
        std::memcpy(&initialize, body + 0x68, 4);
        std::memcpy(&store, body + 0x6c, 4);
        const auto frame_size = (frame >> 10) & 0xfff;
        const auto slot = ((store >> 10) & 0xfff) * 4;
        if ((frame & 0xffc003ff) != 0xd10003ff || initialize != 0x12800000 ||
            (store & 0xffc003ff) != 0xb90003e0 || slot + 4 > frame_size || slot + 32 > 0x3ffc)
            throw std::runtime_error("native beam stack layout changed");
        result = resolve_gate_reviewed(GateKind::Beam, bytes, contract.context);
        const uint32_t load = 0xb94003f0 | (((slot + 32) / 4) << 10);
        std::memcpy(result->payload.data() + 16, &load, 4);
    }
    if (!result)
        throw std::runtime_error("native beam method signature mismatch");
    return *result;
}
GateLayout resolve_gate_layout(GateKind kind, const std::string &bytes) {
    if (kind == GateKind::Input)
        return resolve_gate_reviewed(
            kind, bytes, "790dac44ec743f59cb5f9d8498de276e4fb8969228ee2e205c7f416591e30db8");
    if (kind != GateKind::Beam)
        throw std::invalid_argument("unknown native gate kind");
    return resolve_beam_reviewed(
        bytes, {{0xc30, 8248, "5b41ef23f2f75fcb08bf07951aab21198c1b2e930b6650d56195cc7390f72b98",
                 "0ac3c32eda0053d2026c612513a70f53f5fb152baf761e8bc8a38e34e4ddd3e9"},
                {0xc38, 8256, "31ba833270ee0fdba8acfdaadce9bd33ecbefc51e80035d822fd1c4a02ec2552",
                 "274ec840321f1525d5747ee8c3292852111e7d5059a13534f854542e50858de8"}});
}
void validate_gate_mappings(const GateLayout &layout, uintptr_t base,
                            const std::vector<MemoryMapping> &mappings) {
    const auto kind = layout.kind;
    const auto &binary = kind == GateKind::Input ? steamvr::compatibility::SteamWebHelper
                                                 : steamvr::compatibility::Compositor;
    const auto count = std::count_if(mappings.begin(), mappings.end(), [&](const auto &mapping) {
        return mapping.path == binary.path && !mapping.offset && mapping.readable &&
               mapping.executable && !mapping.writable && mapping.private_mapping &&
               mapping.first == base;
    });
    if (count != 1)
        throw std::runtime_error("expected one executable zero-offset mapping");
    if (base > UINTPTR_MAX - std::max(layout.site + 12, layout.cave + layout.payload.size()))
        throw std::invalid_argument("native gate address overflow");
    for (const auto &range : std::array<std::pair<uintptr_t, size_t>, 2>{
             {{layout.site, 12}, {layout.cave, layout.payload.size()}}})
        if (!std::any_of(mappings.begin(), mappings.end(), [&](const auto &mapping) {
                return mapping.path == binary.path && mapping.readable && mapping.executable &&
                       !mapping.writable && mapping.private_mapping &&
                       mapping.first >= mapping.offset && mapping.first - mapping.offset == base &&
                       mapping.contains(base + range.first, range.second);
            }))
            throw std::runtime_error("patch range is outside the pinned RX mapping");
}
std::optional<uintptr_t> gate_resume_pc(const GateLayout &layout, uintptr_t base, uintptr_t pc) {
    if (layout.kind == GateKind::Beam)
        return std::nullopt;
    if (base > UINTPTR_MAX - layout.cave ||
        base + layout.cave > UINTPTR_MAX - layout.payload.size() ||
        base > UINTPTR_MAX - layout.site - 8)
        throw std::invalid_argument("native gate address overflow");
    if (pc < base + layout.cave || pc - base - layout.cave >= layout.payload.size())
        return std::nullopt;
    const auto offset = pc - base - layout.cave;
    if (offset % 4 || offset == 28)
        throw std::runtime_error("unaligned PC in notification gate");
    return base + layout.site + (offset == 0 ? 0 : offset == 24 ? 8 : 4);
}
bool owned_gate(const GateLayout &layout, const Bytes &value) {
    if (value.size() != layout.payload.size())
        return false;
    for (size_t offset = 0; offset < layout.index_offset; offset += 4) {
        const auto current = u32(value, offset);
        if (current && current != u32(layout.payload, offset))
            return false;
    }
    const auto index = u32(value, layout.index_offset);
    return index == UINT32_MAX || index < layout.index_limit;
}
void validate_gate_start(const GateLayout &layout, const Bytes &site, const Bytes &cave,
                         bool restoring) {
    auto original = layout.original;
    original.insert(original.end(), layout.following.begin(), layout.following.end());
    auto patched = layout.redirect;
    patched.insert(patched.end(), layout.following.begin(), layout.following.end());
    const bool clean =
        site == original && (cave == Bytes(layout.payload.size()) ||
                             (layout.kind == GateKind::Beam && cave == layout.payload));
    if (!clean && !(restoring && (site == original || site == patched) && owned_gate(layout, cave)))
        throw std::runtime_error("Steam UI input code differs from the supported original.");
}
uint32_t validate_live_gate(const GateLayout &layout, const Bytes &site, const Bytes &cave) {
    auto expected = layout.redirect;
    expected.insert(expected.end(), layout.following.begin(), layout.following.end());
    if (site != expected || cave.size() != layout.payload.size() ||
        !std::equal(cave.begin(), cave.begin() + layout.index_offset, layout.payload.begin()))
        throw std::runtime_error("Steam UI input gate changed externally.");
    const auto index = u32(cave, layout.index_offset);
    if (index != UINT32_MAX && index >= layout.index_limit)
        throw std::runtime_error("Steam UI input gate index changed externally.");
    return index;
}
GatePatch::GatePatch(PatchMemory &memory, const GateLayout &layout, uintptr_t base)
    : memory_(memory), layout_(layout), base_(base) {
    if (base % 4096 ||
        base > UINTPTR_MAX - std::max(layout_.site + 12, layout_.cave + layout_.payload.size()))
        throw std::invalid_argument("invalid native gate base");
}
void GatePatch::context() {
    if (memory_.read(base_ + layout_.site + 4, layout_.following.size()) != layout_.following)
        throw std::runtime_error("notification call context mismatch");
}
void GatePatch::apply() {
    context();
    if (memory_.read(base_ + layout_.site, 4) != layout_.original)
        throw std::runtime_error("notification site is not original");
    const auto before = memory_.read(base_ + layout_.cave, layout_.payload.size());
    if (before != Bytes(before.size()) &&
        !(layout_.kind == GateKind::Beam && before == layout_.payload))
        throw std::runtime_error("notification padding is not empty");
    try {
        if (before != layout_.payload)
            patch_words(memory_, PatchStorage::Code, base_ + layout_.cave, layout_.payload);
        patch_words(memory_, PatchStorage::Code, base_ + layout_.site, layout_.redirect);
    } catch (...) {
        const auto failure = std::current_exception();
        try {
            restore([] {});
        } catch (const std::exception &error) {
            throw std::runtime_error(std::string("Native gate rollback failed: ") + error.what());
        }
        std::rethrow_exception(failure);
    }
}
void GatePatch::restore(const std::function<void()> &evacuate) {
    context();
    const auto site = memory_.read(base_ + layout_.site, 4);
    if (site != layout_.original && site != layout_.redirect)
        throw std::runtime_error("notification site is not original or owned");
    const auto cave = memory_.read(base_ + layout_.cave, layout_.payload.size());
    if (!owned_gate(layout_, cave))
        throw std::runtime_error("notification cave is not original or owned");
    patch_words(memory_, PatchStorage::Code, base_ + layout_.site, layout_.original);
    if (layout_.kind == GateKind::Beam) {
        if (!std::equal(cave.begin(), cave.begin() + layout_.index_offset, layout_.payload.begin()))
            patch_words(memory_, PatchStorage::Code, base_ + layout_.cave, layout_.payload);
        else
            patch_words(memory_, PatchStorage::Code, base_ + layout_.cave + layout_.index_offset,
                        word(UINT32_MAX));
    } else {
        evacuate();
        patch_words(memory_, PatchStorage::Code, base_ + layout_.cave,
                    Bytes(layout_.payload.size()));
    }
}
} // namespace ct::helper
