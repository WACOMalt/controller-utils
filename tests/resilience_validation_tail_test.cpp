#include "steamvr/image.h"
#include <cassert>
#include <cstdio>

struct Fixture {
    std::string bytes = std::string(12288, '\0');
    Elf64_Ehdr header{};
    Elf64_Phdr segments[2]{};
    Elf64_Shdr sections[2]{};
    Fixture() {
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
        segments[0].p_type = PT_LOAD;
        segments[0].p_flags = PF_R | PF_X;
        segments[0].p_filesz = 0x900;
        segments[0].p_memsz = 0x900;
        segments[1].p_type = PT_LOAD;
        segments[1].p_flags = PF_R;
        segments[1].p_vaddr = 0x2000;
        segments[1].p_offset = 0x2000;
        segments[1].p_filesz = 0x100;
        segments[1].p_memsz = 0x100;
        sections[1].sh_type = SHT_STRTAB;
        sections[1].sh_offset = 0x800;
        sections[1].sh_size = 1;
    }
    void store() {
        std::memcpy(bytes.data(), &header, sizeof(header));
        std::memcpy(bytes.data() + header.e_phoff, segments, sizeof(segments));
        std::memcpy(bytes.data() + header.e_shoff, sections, sizeof(sections));
    }
    bool accepts(uintptr_t address = 0xa00, size_t size = 16, size_t page = 4096) {
        store();
        try {
            ct::steamvr::Image image(bytes);
            return image.mapped_tail(address, size, page);
        } catch (const std::runtime_error &) {
            return false;
        }
    }
};
int main() {
    Fixture f;
    assert(f.accepts());
    f.store();
    ct::steamvr::Image image(f.bytes);
    try {
        (void)image.at(0xa00, 16, true);
        assert(false);
    } catch (const std::runtime_error &) {
    }
    assert(!f.accepts(0xa00, 0));
    assert(!f.accepts(0xa00, 16, 8192));
    assert(!f.accepts(UINTPTR_MAX - 3, 16));
    f.bytes[0xa00] = 1;
    assert(!f.accepts());
    f.bytes[0xa00] = 0;
    f.sections[0].sh_type = SHT_PROGBITS;
    f.sections[0].sh_offset = 0xa00;
    f.sections[0].sh_size = 16;
    assert(!f.accepts());
    f.sections[0] = {};
    f.header.e_shoff = 0xa00;
    assert(!f.accepts());
    f.header.e_shoff = 512;
    f.segments[1].p_vaddr = 0xb00;
    f.segments[1].p_offset = 0xb00;
    assert(!f.accepts());
    f.segments[1].p_vaddr = 0x2000;
    f.segments[1].p_offset = 0x2000;
    f.segments[0].p_flags = PF_R | PF_W;
    assert(!f.accepts());
    f.segments[0].p_flags = PF_R | PF_X;
    f.segments[1].p_filesz = f.segments[1].p_memsz = 0;
    f.segments[1].p_offset = 0;
    f.bytes.resize(0xa08);
    assert(!f.accepts());
    std::puts("mapped tail validation passed");
}
