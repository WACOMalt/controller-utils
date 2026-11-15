#include "adapter_patch.h"
#include "native_paths.h"
#include "platform/linux/files.h"
#include "steamvr/compatibility.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace ct::helper {
namespace {
constexpr std::array<uintptr_t, 13> Sites{0xeafc50,  0xeb6f30, 0xeb17a0,  0xeb0b00, 0xeb8250,
                                          0xeb7fe0,  0xeb3880, 0x2637680, 0xeb4280, 0xec2270,
                                          0x25e6a70, 0xed358c, 0xf2d888};
template <class T> T field(const Bytes &bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
        throw std::invalid_argument("truncated native adapter field");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
Bytes hex(json_object *value) {
    const auto text = json::string(value);
    if (text.size() % 2 || text.size() > 8192)
        throw std::invalid_argument("invalid adapter hex payload");
    Bytes out;
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return unsigned(c - '0');
        if (c >= 'a' && c <= 'f')
            return unsigned(c - 'a' + 10);
        throw std::invalid_argument("invalid adapter hex payload");
    };
    for (size_t i = 0; i < text.size(); i += 2)
        out.push_back(uint8_t(digit(text[i]) * 16 + digit(text[i + 1])));
    return out;
}
Bytes initial() {
    Bytes bytes(160);
    const auto magic = AdapterPatch::Magic;
    const uint32_t version = 1;
    std::memcpy(bytes.data(), &magic, 8);
    std::memcpy(bytes.data() + 8, &version, 4);
    return bytes;
}
bool partial(const Bytes &value, const Bytes &before, const Bytes &after) {
    if (value.size() != before.size() || before.size() != after.size() || value.size() % 4)
        return false;
    for (size_t i = 0; i < value.size(); i += 4)
        if (field<uint32_t>(value, i) != field<uint32_t>(before, i) &&
            field<uint32_t>(value, i) != field<uint32_t>(after, i))
            return false;
    return true;
}
void validate(const AdapterLayout &layout) {
    if (layout.sites.size() != Sites.size() || layout.loader.empty() ||
        layout.loader.size() > 1104 || layout.loader.size() % 4 ||
        layout.cave_original.size() != layout.loader.size())
        throw std::invalid_argument("invalid native adapter layout");
    for (size_t i = 0; i < Sites.size(); ++i) {
        const auto &site = layout.sites[i];
        if (site.address != Sites[i] || site.original.size() != 16 || site.redirect.size() != 4)
            throw std::invalid_argument("invalid native adapter site");
        const auto instruction = field<uint32_t>(site.redirect, 0);
        const int64_t delta = int64_t(int32_t(instruction << 6)) >> 4;
        const auto destination = int64_t(site.address) + delta;
        if ((instruction & 0xfc000000u) != 0x14000000u ||
            destination < int64_t(AdapterPatch::Cave) ||
            destination >= int64_t(AdapterPatch::Cave + layout.loader.size()))
            throw std::invalid_argument("native adapter redirect leaves the approved loader");
    }
}
} // namespace
AdapterLayout AdapterLayout::from_json(json_object *manifest, const Bytes &loader) {
    if (json::integer(json::field(manifest, "version")) != 1 ||
        json::unsigned_integer(json::field(manifest, "control")) != AdapterPatch::Control ||
        json::unsigned_integer(json::field(manifest, "cave")) != AdapterPatch::Cave ||
        json::string(json::field(manifest, "library_path")) != TrackerLibrary ||
        json::string(json::field(manifest, "xr_sha256")) !=
            steamvr::compatibility::XrService.sha256)
        throw std::invalid_argument("unsupported native adapter layout");
    if (loader.empty() || loader.size() > 1104 || loader.size() % 4)
        throw std::invalid_argument("invalid native adapter loader size");
    if (sha256_bytes(loader.data(), loader.size()) !=
        json::string(json::field(manifest, "loader_sha256")))
        throw std::invalid_argument("native adapter loader checksum mismatch");
    AdapterLayout layout;
    layout.loader = loader;
    layout.cave_original = hex(json::field(manifest, "cave_original"));
    if (layout.cave_original.size() != loader.size())
        throw std::invalid_argument("native loader padding size mismatch");
    auto sites = json::field(manifest, "sites");
    if (json::array_size(sites) != Sites.size())
        throw std::invalid_argument("native adapter site count mismatch");
    for (size_t i = 0; i < Sites.size(); ++i) {
        auto value = json::item(sites, i);
        const auto address = json::unsigned_integer(json::field(value, "address"));
        if (address != Sites[i])
            throw std::invalid_argument("native adapter site differs from supported layout");
        AdapterSite site{uintptr_t(address), hex(json::field(value, "original")),
                         hex(json::field(value, "redirect"))};
        if (site.original.size() != 16 || site.redirect.size() != 4)
            throw std::invalid_argument("native adapter entry size mismatch");
        layout.sites.push_back(std::move(site));
    }
    validate(layout);
    return layout;
}
AdapterPatch::AdapterPatch(PatchMemory &memory, AdapterLayout layout)
    : memory_(memory), layout_(std::move(layout)) {
    validate(layout_);
}
bool AdapterPatch::install() {
    const auto state = memory_.read(Control, 160);
    const bool fresh = state == Bytes(160);
    if (!fresh) {
        const auto phase = field<uint32_t>(state, 112);
        if (field<uint64_t>(state, 0) != Magic || field<uint32_t>(state, 8) != 1 ||
            field<uint32_t>(state, 12) || (phase != 0 && phase != 4 && phase != 5) ||
            field<uint32_t>(state, 120) || field<uint64_t>(state, 80) ||
            field<uint64_t>(state, 88) || field<uint64_t>(state, 96))
            throw std::runtime_error(
                "resident adapter is active or not safely reusable. Restart SteamVR");
        if (field<uint32_t>(state, 116) == 3)
            throw std::runtime_error("Native worker creation failed. Restart SteamVR");
        if (field<uint32_t>(state, 156))
            throw std::runtime_error("Private native worker cannot restart. Restart SteamVR");
    }
    for (const auto &site : layout_.sites) {
        auto expected = site.original;
        if (!fresh)
            std::copy(site.redirect.begin(), site.redirect.end(), expected.begin());
        if (memory_.read(site.address, 16) != expected)
            throw std::runtime_error("native entry fingerprint mismatch");
    }
    if (memory_.read(Cave, layout_.loader.size()) !=
        (fresh ? layout_.cave_original : layout_.loader))
        throw std::runtime_error("native loader padding is not original or this exact adapter");
    if (memory_.read(Camera, 8) != CameraPatch::Original)
        throw std::runtime_error("native camera predicate is not original");
    try {
        if (fresh) {
            patch_words(memory_, PatchStorage::Data, Control, initial());
            patch_words(memory_, PatchStorage::Code, Cave, layout_.loader);
            for (const auto &site : layout_.sites)
                patch_words(memory_, PatchStorage::Code, site.address, site.redirect);
        }
        CameraPatch(memory_).apply();
    } catch (...) {
        const auto failure = std::current_exception();
        if (fresh)
            rollback_install();
        else
            restore_camera();
        std::rethrow_exception(failure);
    }
    return fresh;
}
void AdapterPatch::verify_unarmed() {
    const auto state = memory_.read(Control, 160);
    if (field<uint32_t>(state, 12) ||
        std::any_of(state.begin() + 24, state.end(), [](uint8_t value) { return value != 0; }))
        throw std::runtime_error(
            "cannot remove an adapter which may have executed. Restart SteamVR");
    for (const auto &site : layout_.sites) {
        const Bytes before(site.original.begin(), site.original.begin() + 4);
        if (!partial(memory_.read(site.address, 4), before, site.redirect) ||
            memory_.read(site.address + 4, 12) !=
                Bytes(site.original.begin() + 4, site.original.end()))
            throw std::runtime_error("unarmed adapter range changed externally");
    }
    if (!partial(memory_.read(Cave, layout_.loader.size()), layout_.cave_original,
                 layout_.loader) ||
        !partial(state, Bytes(160), initial()) ||
        !partial(memory_.read(Camera, 8), CameraPatch::Original, CameraPatch::Corrected))
        throw std::runtime_error("unarmed adapter range changed externally");
}
void AdapterPatch::rollback_install() {
    verify_unarmed();
    for (const auto &site : layout_.sites)
        patch_words(memory_, PatchStorage::Code, site.address,
                    Bytes(site.original.begin(), site.original.begin() + 4));
    patch_words(memory_, PatchStorage::Code, Cave, layout_.cave_original);
    patch_words(memory_, PatchStorage::Data, Control, Bytes(160));
    patch_words(memory_, PatchStorage::Code, Camera, CameraPatch::Original);
}
void AdapterPatch::recover_unarmed() {
    verify_unarmed();
    if (memory_.read(Control, 160) == Bytes(160) &&
        memory_.read(Cave, layout_.loader.size()) == layout_.cave_original &&
        memory_.read(Camera, 8) == CameraPatch::Original &&
        std::all_of(layout_.sites.begin(), layout_.sites.end(), [&](const auto &site) {
            return memory_.read(site.address, 16) == site.original;
        }))
        return;
    const bool routed =
        std::any_of(layout_.sites.begin(), layout_.sites.end(), [&](const auto &site) {
            return memory_.read(site.address, 4) == site.redirect;
        });
    if (routed && memory_.read(Cave, layout_.loader.size()) != layout_.loader)
        throw std::runtime_error("native loader was published before its code was complete");
    // A stopped thread may be in an unarmed trampoline. Preserve that immutable code.
    patch_words(memory_, PatchStorage::Data, Control, initial());
    patch_words(memory_, PatchStorage::Code, Cave, layout_.loader);
    for (const auto &site : layout_.sites)
        patch_words(memory_, PatchStorage::Code, site.address, site.redirect);
    patch_words(memory_, PatchStorage::Code, Camera, CameraPatch::Original);
}
void AdapterPatch::restore_camera() {
    CameraPatch(memory_).restore();
}

void validate_adapter_mappings(const std::vector<MemoryMapping> &mappings, size_t loader_size) {
    if (!loader_size || loader_size > 1104)
        throw std::invalid_argument("invalid native adapter loader size");
    auto rx = [&](uintptr_t address, size_t size) {
        return std::any_of(mappings.begin(), mappings.end(), [&](const auto &mapping) {
            return mapping.path == steamvr::compatibility::XrService.path && mapping.readable &&
                   mapping.executable && !mapping.writable && mapping.private_mapping &&
                   mapping.contains(address, size);
        });
    };
    if (!rx(AdapterPatch::Cave, loader_size) || !rx(AdapterPatch::Camera, 8) ||
        !std::all_of(Sites.begin(), Sites.end(), [&](uintptr_t site) { return rx(site, 16); }))
        throw std::runtime_error("native adapter code lies outside pinned RX mappings");
    if (!std::any_of(mappings.begin(), mappings.end(), [](const auto &mapping) {
            return mapping.first == 0x31eb000 && mapping.last == 0x3245000 &&
                   mapping.offset == 0x2fbb000 && mapping.readable && mapping.writable &&
                   !mapping.executable && mapping.private_mapping &&
                   mapping.path == steamvr::compatibility::XrService.path;
        }))
        throw std::runtime_error("native data mapping differs from the pinned ELF");
    if (!std::any_of(mappings.begin(), mappings.end(), [](const auto &mapping) {
            return mapping.first == 0x3245000 && mapping.last == 0x3440000 && !mapping.offset &&
                   !mapping.device && !mapping.inode && mapping.readable && mapping.writable &&
                   !mapping.executable && mapping.private_mapping && mapping.path.empty();
        }))
        throw std::runtime_error("native control BSS mapping differs from the pinned ELF");
}
bool shared_call_active(StoppedProcess &process) {
    const std::array<std::pair<uintptr_t, uintptr_t>, 6> ranges{{{0xeb3880, 0xeb4218},
                                                                 {0xeb8250, 0xeb8434},
                                                                 {0x2637680, 0x26386a0},
                                                                 {0xeb4280, 0xeb4710},
                                                                 {0xec2270, 0xec3080},
                                                                 {0x25e6a70, 0x25e6e40}}};
    for (auto id : process.threads()) {
        const auto stack = process.stack(id);
        if (!stack)
            return true;
        for (auto pc : *stack)
            if (std::any_of(ranges.begin(), ranges.end(), [&](const auto &range) {
                    return range.first <= pc && pc < range.second;
                }))
                return true;
    }
    return false;
}
} // namespace ct::helper
