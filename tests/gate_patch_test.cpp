#include "helper/patch.h"
#include "helper/gate.h"
#include "patch_memory.h"
#include "steamvr/compatibility.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
void seed(ct::test::PatchMemory &memory, const ct::helper::GateLayout &layout, uintptr_t base) {
    auto site = layout.original;
    site.insert(site.end(), layout.following.begin(), layout.following.end());
    memory.regions[base + layout.site] = site;
    memory.regions[base + layout.cave] = ct::helper::Bytes(layout.payload.size());
}
template <class Function> void rejects(Function fn, const std::string &text) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(text) != std::string::npos;
    }
    assert(failed);
}
} // namespace
int main() {
    using namespace ct::helper;
    constexpr uintptr_t base = 0x10000000;
    assert(aarch64_branch(0x23c8bc, 0xccc40) == 0x17fa40e1);
    assert(aarch64_branch(0x8000000, 0) == 0x16000000);
    rejects([] { aarch64_branch(0, 0x8000000); }, "out of range");
    rejects([] { aarch64_branch(1, 4); }, "out of range");
    for (const auto &layout : {place_gate(GateKind::Input, 0x23c8bc, 0xccc40),
                               place_gate(GateKind::Beam, 0xd5660, 0x5c6d00),
                               place_gate(GateKind::Beam, 0xc529c, 0x5c6d00, 19)}) {
        const auto kind = layout.kind;
        const auto &binary = kind == GateKind::Input ? ct::steamvr::compatibility::SteamWebHelper
                                                     : ct::steamvr::compatibility::Compositor;
        std::vector<ct::MemoryMapping> mappings{
            {base, base + 0x600000, 0, 0, 1, true, false, true, binary.path, true}};
        validate_gate_mappings(layout, base, mappings);
        mappings[0].private_mapping = false;
        rejects([&] { validate_gate_mappings(layout, base, mappings); }, "zero-offset");
        mappings[0].private_mapping = true;
        mappings[0].last = base + layout.cave;
        rejects([&] { validate_gate_mappings(layout, base, mappings); }, "pinned RX");
        assert(layout.index_offset + 4 == layout.payload.size());
        {
            auto original = layout.original;
            original.insert(original.end(), layout.following.begin(), layout.following.end());
            auto patched = layout.redirect;
            patched.insert(patched.end(), layout.following.begin(), layout.following.end());
            const Bytes empty(layout.payload.size());
            validate_gate_start(layout, original, empty, false);
            validate_gate_start(layout, original, empty, true);
            validate_gate_start(layout, patched, layout.payload, true);
            rejects([&] { validate_gate_start(layout, patched, layout.payload, false); },
                    "supported original");
            patched.back() ^= 1;
            rejects([&] { validate_gate_start(layout, patched, layout.payload, true); },
                    "supported original");
            auto partial = layout.payload;
            std::fill(partial.begin(), partial.begin() + 4, 0);
            validate_gate_start(layout, original, partial, true);
            rejects([&] { validate_gate_start(layout, original, partial, false); },
                    "supported original");
            partial[4] ^= 1;
            rejects([&] { validate_gate_start(layout, original, partial, true); },
                    "supported original");
            rejects([&] { validate_gate_start(layout, original, Bytes(4), true); },
                    "supported original");
        }
        {
            ct::test::PatchMemory memory;
            seed(memory, layout, base);
            rejects([&] { GateLease lease(memory, layout, base); }, "changed externally");
            GatePatch patch(memory, layout, base);
            patch.apply();
            GateLease lease(memory, layout, base);
            const auto before = memory.writes;
            lease.disable();
            assert(memory.writes == before && !lease.expires());
            for (uint32_t selected = 0; selected < layout.index_limit; ++selected) {
                lease.renew(selected, 10);
                assert(lease.index() == selected && lease.expires() == 10.75);
                const auto live = memory.read(base + layout.cave, layout.payload.size());
                assert(std::equal(live.begin(), live.begin() + layout.index_offset,
                                  layout.payload.begin()));
                lease.poll(10.749);
                assert(lease.index() == selected);
                lease.poll(10.75);
                assert(lease.index() == UINT32_MAX && !lease.expires());
            }
            lease.renew(0, 11);
            rejects([&] { GateLease other(memory, layout, base); }, "did not start disabled");
            const auto writes = memory.writes;
            rejects([&] { lease.renew(layout.index_limit, 11); }, "controller index");
            rejects([&] { lease.renew(0, std::numeric_limits<double>::infinity()); }, "clock");
            assert(memory.writes == writes);
            memory.fail_at = memory.writes + 1;
            rejects([&] { lease.disable(); }, "injected patch write failure");
            assert(lease.index() == 0 && lease.expires() == 11.75);
            lease.disable();
            for (size_t offset = 0; offset < layout.index_offset; ++offset) {
                memory.regions[base + layout.cave][offset] ^= 1;
                const auto count = memory.writes;
                rejects([&] { lease.renew(0, 12); }, "changed externally");
                assert(memory.writes == count);
                memory.regions[base + layout.cave][offset] ^= 1;
            }
            const uint32_t invalid = layout.index_limit;
            std::memcpy(memory.regions[base + layout.cave].data() + layout.index_offset, &invalid,
                        4);
            rejects([&] { lease.disable(); }, "index changed externally");
        }
        for (bool after : {false, true}) {
            for (size_t failure = 1; failure <= layout.payload.size() / 4 + 1; ++failure) {
                ct::test::PatchMemory memory;
                seed(memory, layout, base);
                memory.fail_at = failure;
                memory.fail_after = after;
                GatePatch patch(memory, layout, base);
                rejects([&] { patch.apply(); }, "injected patch write failure");
                assert(memory.read(base + layout.site, 4) == layout.original);
                assert(memory.read(base + layout.cave, layout.payload.size()) ==
                       (kind == GateKind::Input ? Bytes(layout.payload.size()) : layout.payload));
            }
        }
        {
            ct::test::PatchMemory memory;
            seed(memory, layout, base);
            GatePatch patch(memory, layout, base);
            patch.apply();
            assert(memory.read(base + layout.site, 4) == layout.redirect);
            assert(memory.read(base + layout.cave, layout.payload.size()) == layout.payload);
            const uint32_t selected = layout.index_limit - 1;
            std::memcpy(memory.regions[base + layout.cave].data() + layout.index_offset, &selected,
                        4);
            bool moved = false;
            patch.restore([&] {
                assert(memory.read(base + layout.site, 4) == layout.original);
                moved = true;
            });
            assert(moved == (kind == GateKind::Input));
            assert(memory.read(base + layout.cave, layout.payload.size()) ==
                   (kind == GateKind::Input ? Bytes(layout.payload.size()) : layout.payload));
            patch.apply();
            if (kind == GateKind::Input) {
                rejects([&] { patch.restore([] { throw std::runtime_error("PC move failed"); }); },
                        "PC move failed");
                assert(memory.read(base + layout.site, 4) == layout.original);
                assert(memory.read(base + layout.cave, layout.payload.size()) == layout.payload);
                patch.restore([] {});
            } else
                patch.restore([] {});
        }
        {
            ct::test::PatchMemory memory;
            seed(memory, layout, base);
            memory.regions[base + layout.site][4] ^= 1;
            GatePatch patch(memory, layout, base);
            rejects([&] { patch.apply(); }, "context mismatch");
            assert(!memory.writes);
        }
        {
            ct::test::PatchMemory memory;
            seed(memory, layout, base);
            GatePatch patch(memory, layout, base);
            patch.apply();
            memory.regions[base + layout.cave][0] ^= 1;
            const auto writes = memory.writes;
            rejects([&] { patch.restore([] {}); }, "not original or owned");
            assert(memory.writes == writes);
        }
        auto invalid = layout.payload;
        const uint32_t index = layout.index_limit;
        std::memcpy(invalid.data() + layout.index_offset, &index, 4);
        assert(!owned_gate(layout, invalid));
        assert(owned_gate(layout, Bytes(layout.payload.size())));
    }
    for (const auto &addresses :
         {std::pair<uintptr_t, uintptr_t>{0x1000, 0x4000}, {0x6000, 0x3000}}) {
        const auto beam = place_gate(GateKind::Beam, addresses.first, addresses.second);
        uint32_t redirect, resume;
        std::memcpy(&redirect, beam.redirect.data(), 4);
        std::memcpy(&resume, beam.payload.data() + 52, 4);
        assert(redirect == aarch64_branch(beam.site, beam.cave));
        assert(resume == aarch64_branch(beam.cave + 52, beam.site + 4));
        ct::test::PatchMemory memory;
        seed(memory, beam, base);
        memory.regions[base + beam.cave] = beam.payload;
        GatePatch patch(memory, beam, base);
        patch.apply();
        assert(memory.writes == 1);
        patch.restore([] { assert(false); });
        assert(memory.read(base + beam.cave, beam.payload.size()) == beam.payload);
        memory.regions[base + beam.cave][0] ^= 1;
        const auto writes = memory.writes;
        rejects([&] { patch.apply(); }, "padding is not empty");
        assert(memory.writes == writes);
    }
    rejects([] { place_gate(GateKind::Beam, 0, 0x8000000); }, "out of range");
    rejects([] { place_gate(GateKind::Beam, 0x1001, 0x2000); }, "out of range");
    rejects([] { place_gate(GateKind::Beam, 0, 0, 20); }, "unsupported native beam register");
    rejects([] { place_gate(GateKind::Input, 0, 0, 19); }, "unsupported native beam register");
    const auto input = place_gate(GateKind::Input, 0x23c8bc, 0xccc40);
    assert(!gate_resume_pc(input, base, base + input.cave - 4));
    for (size_t offset = 0; offset < 28; offset += 4)
        assert(gate_resume_pc(input, base, base + input.cave + offset) == base + input.site +
                                                                              (offset == 0    ? 0
                                                                               : offset == 24 ? 8
                                                                                              : 4));
    rejects([&] { gate_resume_pc(input, base, base + input.cave + 28); }, "unaligned PC");
    rejects([&] { gate_resume_pc(input, base, base + input.cave + 1); }, "unaligned PC");
    std::cout << "native gates: exact payloads, every write failure, ownership, PC moves and "
                 "resident beam restoration passed\n";
}
