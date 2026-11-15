#include "../driver/native_queue_startup.h"
#include "../driver/compatibility_profile.h"
#include "steamvr/image.h"
#include <cassert>
#include <array>
#include <cstdio>
#include <type_traits>

static void image_checks() {
    std::string bytes(4096, '\0');
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_version = EV_CURRENT;
    header.e_machine = EM_AARCH64;
    header.e_type = ET_DYN;
    header.e_ehsize = sizeof(header);
    header.e_phoff = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 2;
    header.e_shoff = 512;
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = 2;
    header.e_shstrndx = 1;
    Elf64_Phdr segments[2]{};
    segments[0].p_type = PT_LOAD;
    segments[0].p_flags = PF_R | PF_X;
    segments[0].p_filesz = 4096;
    segments[0].p_memsz = 4096;
    segments[1].p_type = PT_LOAD;
    segments[1].p_flags = PF_R;
    segments[1].p_vaddr = 4096;
    Elf64_Shdr sections[2]{};
    sections[1].sh_type = SHT_STRTAB;
    sections[1].sh_offset = 768;
    sections[1].sh_size = 1;
    auto store = [&] {
        std::memcpy(bytes.data(), &header, sizeof(header));
        std::memcpy(bytes.data() + header.e_phoff, segments, sizeof(segments));
        std::memcpy(bytes.data() + header.e_shoff, sections, sizeof(sections));
    };
    auto rejected = [&] {
        store();
        try {
            ct::steamvr::Image image(bytes);
            return false;
        } catch (const std::runtime_error &) {
            return true;
        }
    };
    store();
    ct::steamvr::Image valid(bytes);
    assert(valid.at(0, 4, true) == bytes.data());
    segments[1].p_memsz = 16;
    segments[1].p_vaddr = 0;
    assert(rejected());
    segments[1].p_vaddr = 4096;
    segments[0].p_flags |= PF_W;
    assert(rejected());
    segments[0].p_flags &= ~PF_W;
    segments[0].p_filesz = 4097;
    assert(rejected());
    segments[0].p_filesz = 4096;
    header.e_phoff = UINT64_MAX;
    std::memcpy(bytes.data(), &header, sizeof(header));
    try {
        ct::steamvr::Image invalid(bytes);
        assert(false);
    } catch (const std::runtime_error &) {
    }
}

static void signature_checks() {
    using ct::steamvr::code_signature;
    std::array<uint32_t, 2> words{0x10000000, 0xd503201f};
    auto initial = code_signature(words.data(), sizeof(words));
    words[0] = 0x10000020;
    assert(code_signature(words.data(), sizeof(words)) != initial);
    words[0] = 0x10000100;
    initial = code_signature(words.data(), sizeof(words));
    words[0] = 0x10000200;
    assert(code_signature(words.data(), sizeof(words)) == initial);
    words[0] = 0x18000000;
    initial = code_signature(words.data(), sizeof(words));
    words[0] = 0x18000020;
    assert(code_signature(words.data(), sizeof(words)) != initial);
    words[0] = 0x18000100;
    initial = code_signature(words.data(), sizeof(words));
    words[0] = 0x18000200;
    assert(code_signature(words.data(), sizeof(words)) == initial);
}
static void profile_checks() {
    using namespace ct::steamvr;
    using namespace ct::queue_startup;
    alignas(8) std::array<uintptr_t, 11> table{};
    alignas(8) std::array<uintptr_t, 2> info{};
    const char name[] = "20CVRBlockQueueManager";
    info[1] = reinterpret_cast<uintptr_t>(name);
    table[1] = reinterpret_cast<uintptr_t>(info.data());
    auto range = [](const void *p, size_t n, bool executable) {
        const auto address = reinterpret_cast<uintptr_t>(p);
        return Module::Range{address, address + n, executable};
    };
    std::vector<Module::Range> ranges{range(table.data(), sizeof(table), false),
                                      range(info.data(), sizeof(info), false),
                                      range(name, sizeof(name), false)};
    std::array<std::vector<uint32_t>, 9> code;
    ct::update::Profile profile{1, 2000, ct::TrackingBackend::Tracker, {}};
    for (size_t i = 0; i < code.size(); ++i) {
        code[i].resize(ServerQueueLayout.methods[i].size / 4, 0xd503201f);
        table[i + 2] = reinterpret_cast<uintptr_t>(code[i].data());
        const auto size = code[i].size() * sizeof(uint32_t);
        ranges.push_back(range(code[i].data(), size, true));
        profile.queue_fingerprints[i] = code_signature(code[i].data(), size);
    }
    Module module(std::move(ranges));
    assert(validate_profile_queue(table.data() + 2, module, profile, ct::TrackingBackend::Tracker));
    assert(!validate_profile_queue(table.data() + 2, module, profile,
                                   static_cast<ct::TrackingBackend>(1)));
    code[0][0] ^= 1;
    assert(
        !validate_profile_queue(table.data() + 2, module, profile, ct::TrackingBackend::Tracker));
}

int main() {
    using namespace ct::steamvr;
    using namespace ct::queue_startup;
    image_checks();
    signature_checks();
    profile_checks();
    static_assert(!std::is_constructible_v<Image, std::string &&>);
    alignas(8) std::array<uintptr_t, 11> table{};
    alignas(8) std::array<uintptr_t, 2> info{};
    const char name[] = "synthetic";
    alignas(8) std::array<uint32_t, 18> code{};
    std::array<std::string, 9> hashes;
    QueueLayout layout{};
    layout.rtti = name;
    info[1] = reinterpret_cast<uintptr_t>(name);
    table[1] = reinterpret_cast<uintptr_t>(info.data());
    for (size_t i = 0; i < 9; ++i) {
        code[i * 2] = 0xd503201f + uint32_t(i);
        hashes[i] = code_signature(&code[i * 2], 8);
        layout.methods[i] = {8, hashes[i].c_str()};
        table[i + 2] = reinterpret_cast<uintptr_t>(&code[i * 2]);
    }
    auto range = [](const void *p, size_t n, bool exec) {
        auto a = reinterpret_cast<uintptr_t>(p);
        return Module::Range{a, a + n, exec};
    };
    Module module({range(table.data(), sizeof(table), false),
                   range(info.data(), sizeof(info), false), range(name, sizeof(name), false),
                   range(code.data(), sizeof(code), true)});
    assert(validate_queue_layout(table.data() + 2, module, layout));
    code[0] ^= 1;
    assert(!validate_queue_layout(table.data() + 2, module, layout));
    code[0] ^= 1;
    auto saved = table[3];
    table[3] = table[2];
    assert(!validate_queue_layout(table.data() + 2, module, layout));
    layout.methods[1].signature = hashes[0].c_str();
    assert(!validate_queue_layout(table.data() + 2, module, layout));
    layout.methods[1].signature = hashes[1].c_str();
    table[3] = saved;
    layout.rtti = "incorrect";
    assert(!validate_queue_layout(table.data() + 2, module, layout));
    assert(!module.type(
        reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(table.data() + 2) + 1), 9, name));
    assert(!module.method(table[2], 0, hashes[0].c_str()));
    try {
        const std::string bytes("bad");
        Image image(bytes);
        assert(false);
    } catch (const std::runtime_error &) {
    }
    std::puts("resilience validation passed");
}
