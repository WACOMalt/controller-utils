#include "steamvr/ui_gate.h"
#include "steamvr/code.h"
#include "steamvr/image.h"
#include <cassert>
#include <cstring>
#include <elf.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace ct::steamvr {
GateLayout resolve_gate_reviewed(GateKind, const std::string &, const char *, unsigned = 11);
}

namespace {
template <class T> void put(std::string &data, size_t offset, const T &value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}
std::string image(size_t window, size_t plt_size = 0x40,
                  ct::steamvr::GateKind kind = ct::steamvr::GateKind::Input) {
    std::string data(0x3200, '\0');
    Elf64_Ehdr h{};
    std::memcpy(h.e_ident, ELFMAG, SELFMAG);
    h.e_ident[EI_CLASS] = ELFCLASS64;
    h.e_ident[EI_DATA] = ELFDATA2LSB;
    h.e_ident[EI_VERSION] = EV_CURRENT;
    h.e_version = EV_CURRENT;
    h.e_type = ET_DYN;
    h.e_machine = EM_AARCH64;
    h.e_ehsize = sizeof(h);
    h.e_phoff = sizeof(h);
    h.e_phentsize = sizeof(Elf64_Phdr);
    h.e_phnum = 1;
    h.e_shoff = 0x3000;
    h.e_shentsize = sizeof(Elf64_Shdr);
    h.e_shnum = 4;
    h.e_shstrndx = 3;
    put(data, 0, h);
    Elf64_Phdr p{};
    p.p_type = PT_LOAD;
    p.p_flags = PF_R | PF_X;
    p.p_filesz = p.p_memsz = kind == ct::steamvr::GateKind::Beam ? 0x2801 : 0x3000;
    p.p_align = 4096;
    put(data, h.e_phoff, p);
    const char names[] = "\0.plt\0.text\0.shstrtab\0";
    std::memcpy(data.data() + 0x200, names, sizeof(names));
    Elf64_Shdr s{};
    s.sh_name = 1;
    s.sh_type = SHT_PROGBITS;
    s.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    s.sh_addr = s.sh_offset = 0x400;
    s.sh_size = plt_size;
    put(data, h.e_shoff + sizeof(s), s);
    s.sh_name = 6;
    s.sh_addr = s.sh_offset = 0x1000;
    s.sh_size = 0x1000;
    put(data, h.e_shoff + 2 * sizeof(s), s);
    s = {};
    s.sh_name = 12;
    s.sh_type = SHT_STRTAB;
    s.sh_offset = 0x200;
    s.sh_size = sizeof(names);
    put(data, h.e_shoff + 3 * sizeof(s), s);
    for (size_t i = 0; i < 128; i += 4)
        put(data, window + i, uint32_t(0xd503201f));
    const auto prototype = ct::steamvr::place_gate(kind, 0, 0);
    const size_t site = window + (kind == ct::steamvr::GateKind::Input ? 44 : 64);
    std::memcpy(data.data() + site, prototype.original.data(), prototype.original.size());
    std::memcpy(data.data() + site + 4, prototype.following.data(), prototype.following.size());
    return data;
}
std::string digest(const std::string &data, size_t window) {
    return ct::steamvr::code_signature(data.data() + window, 128);
}
template <class F> void rejects(F fn) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &) {
        failed = true;
    }
    assert(failed);
}
} // namespace
int main() {
    using namespace ct::steamvr;
    auto first = image(0x1100);
    auto second = image(0x1400, 0x80);
    const auto signature = digest(first, 0x1100);
    rejects([&] { resolve_gate_layout(GateKind::Input, first); });
    auto a = resolve_gate_reviewed(GateKind::Input, first, signature.c_str());
    auto b = resolve_gate_reviewed(GateKind::Input, second, signature.c_str());
    assert(a.site == 0x112c && a.cave == 0x480);
    assert(b.site == 0x142c && b.cave == 0x4c0);
    uint32_t branch;
    std::memcpy(&branch, b.redirect.data(), 4);
    assert(branch == aarch64_branch(b.site, b.cave));
    std::memcpy(&branch, b.payload.data() + 20, 4);
    assert(branch == aarch64_branch(b.cave + 20, b.site + 4));
    auto duplicate = first;
    std::memcpy(duplicate.data() + 0x1500, first.data() + 0x1100, 128);
    rejects([&] { resolve_gate_reviewed(GateKind::Input, duplicate, signature.c_str()); });
    auto changed = first;
    changed[0x1100] ^= 1;
    rejects([&] { resolve_gate_reviewed(GateKind::Input, changed, signature.c_str()); });
    auto following = first;
    following[0x1100 + 48] ^= 1;
    rejects([&] { resolve_gate_reviewed(GateKind::Input, following, signature.c_str()); });
    auto occupied = first;
    occupied[0x480] = 1;
    rejects([&] { resolve_gate_reviewed(GateKind::Input, occupied, signature.c_str()); });
    auto tail = image(0x1100, 0x40, GateKind::Beam);
    const auto beam_signature = digest(tail, 0x1100);
    rejects([&] { resolve_gate_layout(GateKind::Beam, tail); });
    const auto beam = resolve_gate_reviewed(GateKind::Beam, tail, beam_signature.c_str());
    assert(beam.site == 0x1140 && beam.cave == 0x2850);
    auto moved = image(0x1500, 0x40, GateKind::Beam);
    const auto relocated = resolve_gate_reviewed(GateKind::Beam, moved, beam_signature.c_str());
    assert(relocated.site == 0x1540 && relocated.cave == 0x2850);
    std::memcpy(&branch, relocated.payload.data() + 52, 4);
    assert(branch == aarch64_branch(relocated.cave + 52, relocated.site + 4));
    auto duplicate_beam = tail;
    std::memcpy(duplicate_beam.data() + 0x1500, tail.data() + 0x1100, 128);
    rejects([&] { resolve_gate_reviewed(GateKind::Beam, duplicate_beam, beam_signature.c_str()); });
    auto changed_beam = tail;
    changed_beam[0x1100] ^= 1;
    rejects([&] { resolve_gate_reviewed(GateKind::Beam, changed_beam, beam_signature.c_str()); });
    ct::steamvr::Image tail_image(tail);
    const auto tail_cave = tail_image.padding(false, 60);
    assert(tail_cave == 0x2850);
    assert(tail_image.mapped_tail(tail_cave, 60, 4096));
    rejects([&] { tail_image.at(tail_cave, 60, true); });
    tail[tail_cave] = 1;
    rejects([&] { resolve_gate_reviewed(GateKind::Beam, tail, beam_signature.c_str()); });
    rejects([&] { tail_image.mapped_tail(tail_cave, 60, 4096); });
    rejects([] { place_gate(GateKind::Input, 0, 0x8000000); });
    rejects([] { place_gate(GateKind::Input, UINTPTR_MAX - 4, 0); });
    for (const auto &shape : {std::pair{0x13cu, 11u}, {0x14cu, 11u}, {0x14cu, 19u}}) {
        const auto slot = shape.first, beam_register = shape.second;
        auto bytes = image(0x11c0, 0x40, GateKind::Beam);
        for (size_t at = 0x1000; at < 0x1400; at += 4)
            put(bytes, at, uint32_t(0xd503201f));
        put(bytes, 0x1000, uint32_t(0xd10003ff | ((slot + 0x214) << 10)));
        put(bytes, 0x1068, uint32_t(0x12800000));
        put(bytes, 0x106c, uint32_t(0xb90003e0 | ((slot / 4) << 10)));
        const auto prototype = place_gate(GateKind::Beam, 0x1200, 0x2850, beam_register);
        std::memcpy(bytes.data() + 0x1200, prototype.original.data(), 4);
        std::memcpy(bytes.data() + 0x1204, prototype.following.data(), 8);
        const auto body_hash = ct::steamvr::code_signature(bytes.data() + 0x1000, 0x400);
        const auto context_hash = digest(bytes, 0x11c0);
        const std::vector<BeamContract> contracts{
            {0x200, 0x400, body_hash.c_str(), context_hash.c_str(), beam_register}};
        const auto resolved = resolve_beam_reviewed(bytes, contracts);
        std::memcpy(&branch, resolved.payload.data() + 16, 4);
        assert(branch == (0xb94003f0 | (((slot + 32) / 4) << 10)));
        std::memcpy(&branch, resolved.payload.data() + 12, 4);
        assert(branch == (0xbd441280 | beam_register));
        std::memcpy(&branch, resolved.payload.data() + 36, 4);
        assert(branch == (0x1e2703e0 | beam_register));
        rejects([&] { resolve_gate_layout(static_cast<GateKind>(99), bytes); });
        rejects([&] {
            resolve_beam_reviewed(
                bytes, {{0x200, 16388, body_hash.c_str(), context_hash.c_str(), beam_register}});
        });
        auto unrelated = bytes;
        unrelated += "unrelated vendor update";
        assert(resolve_beam_reviewed(unrelated, contracts).payload == resolved.payload);
        auto altered = bytes;
        altered[0x1080] ^= 1;
        rejects([&] { resolve_beam_reviewed(altered, contracts); });
        rejects([&] { resolve_beam_reviewed(bytes, {contracts[0], contracts[0]}); });
        altered = bytes;
        std::memcpy(altered.data() + 0x1500, prototype.original.data(), 4);
        std::memcpy(altered.data() + 0x1504, prototype.following.data(), 8);
        rejects([&] { resolve_beam_reviewed(altered, contracts); });
        altered = bytes;
        altered[0x2850] = 1;
        rejects([&] { resolve_beam_reviewed(altered, contracts); });
        rejects([&] { resolve_beam_reviewed(bytes.substr(0, 0x1200), contracts); });
        altered = bytes;
        put(altered, 0x106c, uint32_t(0xb90003e0 | (0x3ff << 10)));
        const auto invalid_hash = ct::steamvr::code_signature(altered.data() + 0x1000, 0x400);
        rejects([&] {
            resolve_beam_reviewed(altered, {{0x200, 0x400, invalid_hash.c_str(),
                                             context_hash.c_str(), beam_register}});
        });
    }
    std::cout << "gate discovery relocation and rejection passed\n";
}
