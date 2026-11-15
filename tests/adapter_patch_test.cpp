#include "helper/adapter_patch.h"
#include "patch_memory.h"
#include "platform/linux/files.h"
#include "steamvr/compatibility.h"
#include <cassert>
#include <cstring>
#include <iostream>

namespace {
ct::helper::Bytes loader() {
    const auto text = ct::read_file("tests/fixtures/native-loader.hex", 4096);
    ct::helper::Bytes bytes;
    for (size_t i = 0; i + 1 < text.size(); i += 2)
        bytes.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
    return bytes;
}
void seed(ct::test::PatchMemory &memory, const ct::helper::AdapterLayout &layout) {
    for (const auto &site : layout.sites)
        memory.regions[site.address] = site.original;
    memory.regions[ct::helper::AdapterPatch::Cave] = layout.cave_original;
    memory.regions[ct::helper::AdapterPatch::Control] = ct::helper::Bytes(160);
    memory.regions[ct::helper::AdapterPatch::Camera] = {0x00, 0xd4, 0x60, 0x1e,
                                                        0xa7, 0xfd, 0xff, 0x54};
}
template <class Function> void rejects(Function fn, const std::string &message) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(message) != std::string::npos;
    }
    assert(failed);
}
} // namespace
int main() {
    using namespace ct::helper;
    const auto manifest =
        ct::json::parse(ct::read_file("tests/fixtures/native-adapter.json", 32768));
    for (const auto runtime : ct::steamvr::compatibility::XrServiceHashes) {
        const auto layout = AdapterLayout::from_json(manifest.get(), loader(), runtime);
        rejects([&] { AdapterLayout::from_json(manifest.get(), loader(), std::string(64, '0')); },
                "unsupported");
        {
            auto other = ct::json::parse(ct::json::encode(manifest.get()));
            const auto originals = ct::json::field(other.get(), "runtime_originals");
            json_object_object_add(originals, ct::steamvr::compatibility::XrServiceHashes[1],
                                   json_object_new_string("0000"));
            rejects([&] { AdapterLayout::from_json(other.get(), loader(), runtime); },
                    "padding size");
        }
        const auto path = std::string(ct::steamvr::compatibility::XrService.path);
        auto mappings = ct::process_mappings("00200000-031eb000 r-xp 00000000 00:01 1 " + path +
                                             "\n031eb000-03245000 rw-p 02fbb000 00:01 1 " + path +
                                             "\n03245000-03440000 rw-p 00000000 00:00 0\n");
        validate_adapter_mappings(mappings, layout.loader.size());
        mappings[0].private_mapping = false;
        rejects([&] { validate_adapter_mappings(mappings, layout.loader.size()); }, "pinned RX");
        mappings[0].private_mapping = true;
        mappings[2].last -= 4096;
        rejects([&] { validate_adapter_mappings(mappings, layout.loader.size()); }, "control BSS");
        const auto writes = 40 + layout.loader.size() / 4 + layout.sites.size() + 2;
        {
            ct::test::PatchMemory memory;
            seed(memory, layout);
            const auto original = memory.regions;
            AdapterPatch patch(memory, layout);
            patch.recover_unarmed();
            assert(memory.regions == original && !memory.writes);
        }
        for (bool after : {false, true}) {
            for (size_t failure = 1; failure <= writes; ++failure) {
                ct::test::PatchMemory memory;
                seed(memory, layout);
                const auto original = memory.regions;
                memory.fail_at = failure;
                memory.fail_after = after;
                AdapterPatch patch(memory, layout);
                rejects([&] { patch.install(); }, "injected patch write failure");
                assert(memory.regions == original);
            }
        }
        {
            ct::test::PatchMemory memory;
            seed(memory, layout);
            AdapterPatch patch(memory, layout);
            assert(patch.install());
            assert(memory.read(AdapterPatch::Cave, layout.loader.size()) == layout.loader);
            for (const auto &site : layout.sites)
                assert(memory.read(site.address, 4) == site.redirect);
            memory.regions[AdapterPatch::Control][24] = 1;
            auto before = memory.regions;
            const auto count = memory.writes;
            rejects([&] { patch.recover_unarmed(); }, "may have executed");
            assert(memory.writes == count && memory.regions == before);
            patch.restore_camera();
            memory.regions[AdapterPatch::Control][112] = 4;
            assert(!patch.install());
            patch.restore_camera();
            memory.regions[AdapterPatch::Control][116] = 3;
            const auto owned = memory.regions;
            rejects([&] { patch.install(); }, "worker creation failed");
            assert(memory.regions == owned);
            memory.regions[AdapterPatch::Control][116] = 0;
            memory.regions[AdapterPatch::Control][152] = 1;
            assert(!patch.install());
            patch.restore_camera();
            memory.regions[AdapterPatch::Control][156] = 1;
            const auto stopped_worker = memory.regions;
            rejects([&] { patch.install(); }, "cannot restart");
            assert(memory.regions == stopped_worker);
            memory.regions[AdapterPatch::Control][156] = 0;
            memory.regions[AdapterPatch::Control][88] = 1;
            before = memory.regions;
            rejects([&] { patch.install(); }, "not safely reusable");
            assert(memory.regions == before);
        }
        {
            ct::test::PatchMemory memory;
            seed(memory, layout);
            AdapterPatch patch(memory, layout);
            assert(patch.install());
            patch.recover_unarmed();
            assert(memory.read(AdapterPatch::Cave, layout.loader.size()) == layout.loader);
            assert(!patch.install());
        }
        {
            ct::test::PatchMemory memory;
            seed(memory, layout);
            const auto &site = layout.sites[0];
            std::copy(site.redirect.begin(), site.redirect.end(),
                      memory.regions[site.address].begin());
            const auto before = memory.regions;
            AdapterPatch patch(memory, layout);
            rejects([&] { patch.recover_unarmed(); }, "before its code was complete");
            assert(memory.regions == before && !memory.writes);
        }
        {
            ct::test::PatchMemory memory;
            seed(memory, layout);
            AdapterPatch patch(memory, layout);
            patch.install();
            memory.regions[AdapterPatch::Cave][0] ^= 1;
            const auto before = memory.regions;
            rejects([&] { patch.recover_unarmed(); }, "changed externally");
            assert(memory.regions == before);
        }
        {
            auto invalid = ct::json::parse(ct::json::encode(manifest.get()));
            json_object_object_add(invalid.get(), "control", json_object_new_int(0));
            rejects([&] { AdapterLayout::from_json(invalid.get(), loader(), runtime); },
                    "unsupported");
            rejects([&] { AdapterLayout::from_json(manifest.get(), Bytes(1108), runtime); },
                    "loader size");
            auto changed = loader();
            changed[0] ^= 1;
            rejects([&] { AdapterLayout::from_json(manifest.get(), changed, runtime); },
                    "checksum mismatch");
            auto invalid_layout = layout;
            invalid_layout.sites[0].redirect = {0, 0, 0, 0};
            ct::test::PatchMemory memory;
            rejects([&] { AdapterPatch patch(memory, invalid_layout); }, "approved loader");
        }
    }
    std::cout << "native adapter: pinned layout, every write failure, resident reuse and safe "
                 "interrupted recovery passed\n";
}
