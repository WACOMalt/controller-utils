#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <elf.h>

namespace ct::steamvr {
class Image {
    const std::string &bytes_;
    std::vector<Elf64_Phdr> segments_;
    std::vector<Elf64_Shdr> sections_;
    Elf64_Shdr names_{};
    Elf64_Ehdr header_{};
    template <class T> T read(size_t offset) const {
        T value;
        std::memcpy(&value, bytes(offset, sizeof(T)), sizeof(T));
        return value;
    }
    const char *bytes(size_t offset, size_t size) const {
        if (offset > bytes_.size() || size > bytes_.size() - offset)
            throw std::runtime_error("Truncated native image");
        return bytes_.data() + offset;
    }

  public:
    explicit Image(const std::string &&) = delete;
    explicit Image(const std::string &value) : bytes_(value) {
        const auto header = read<Elf64_Ehdr>(0);
        header_ = header;
        if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) ||
            header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
            header.e_ident[EI_VERSION] != EV_CURRENT || header.e_version != EV_CURRENT ||
            header.e_machine != EM_AARCH64 ||
            (header.e_type != ET_DYN && header.e_type != ET_EXEC) ||
            header.e_ehsize != sizeof(Elf64_Ehdr) || header.e_phentsize != sizeof(Elf64_Phdr) ||
            header.e_shentsize != sizeof(Elf64_Shdr) || !header.e_phnum || header.e_phnum > 128 ||
            !header.e_shnum || header.e_shnum > 512 || header.e_shstrndx >= header.e_shnum)
            throw std::runtime_error("Unsupported native image layout");
        bytes(header.e_phoff, size_t(header.e_phnum) * sizeof(Elf64_Phdr));
        bytes(header.e_shoff, size_t(header.e_shnum) * sizeof(Elf64_Shdr));
        for (size_t i = 0; i < header.e_phnum; ++i) {
            auto segment = read<Elf64_Phdr>(header.e_phoff + i * sizeof(Elf64_Phdr));
            bytes(segment.p_offset, segment.p_filesz);
            if (segment.p_filesz > segment.p_memsz ||
                segment.p_vaddr > UINT64_MAX - segment.p_memsz)
                throw std::runtime_error("Invalid native image segment");
            if (segment.p_type == PT_LOAD) {
                if (segment.p_offset % 4096 != segment.p_vaddr % 4096 ||
                    ((segment.p_flags & PF_W) && (segment.p_flags & PF_X)))
                    throw std::runtime_error("Invalid native image permissions");
                for (const auto &other : segments_)
                    if (segment.p_memsz && other.p_memsz &&
                        segment.p_vaddr < other.p_vaddr + other.p_memsz &&
                        other.p_vaddr < segment.p_vaddr + segment.p_memsz)
                        throw std::runtime_error("Overlapping native image segments");
                segments_.push_back(segment);
            }
        }
        for (size_t i = 0; i < header.e_shnum; ++i) {
            auto section = read<Elf64_Shdr>(header.e_shoff + i * sizeof(Elf64_Shdr));
            if (section.sh_type != SHT_NOBITS)
                bytes(section.sh_offset, section.sh_size);
            if (section.sh_addr > UINT64_MAX - section.sh_size)
                throw std::runtime_error("Invalid native image section");
            sections_.push_back(section);
        }
        names_ = sections_[header.e_shstrndx];
        if (names_.sh_type != SHT_STRTAB)
            throw std::runtime_error("Invalid native image section names");
    }
    Elf64_Shdr section(const std::string &name) const {
        std::optional<Elf64_Shdr> result;
        for (const auto &section : sections_) {
            if (section.sh_name >= names_.sh_size)
                throw std::runtime_error("Invalid native image section name");
            const auto text =
                bytes(names_.sh_offset + section.sh_name, names_.sh_size - section.sh_name);
            if (!std::memchr(text, 0, names_.sh_size - section.sh_name))
                throw std::runtime_error("Unterminated native image section name");
            if (text == name) {
                if (result)
                    throw std::runtime_error("Ambiguous native image section");
                result = section;
            }
        }
        if (!result)
            throw std::runtime_error("Missing native image section");
        return *result;
    }
    const char *at(uintptr_t address, size_t size, bool executable = false) const {
        const Elf64_Phdr *match = nullptr;
        for (const auto &segment : segments_)
            if (address >= segment.p_vaddr && address - segment.p_vaddr <= segment.p_filesz &&
                size <= segment.p_filesz - (address - segment.p_vaddr) &&
                (segment.p_flags & PF_R) &&
                (!executable || ((segment.p_flags & PF_X) && !(segment.p_flags & PF_W)))) {
                if (match)
                    throw std::runtime_error("Ambiguous native image mapping");
                match = &segment;
            }
        if (!match)
            throw std::runtime_error("Native image address is outside its segment");
        return bytes(match->p_offset + address - match->p_vaddr, size);
    }
    const char *mapped_tail(uintptr_t address, size_t size, size_t page_size) const {
        if ((page_size != 4096 && page_size != 16384 && page_size != 65536) || !size ||
            address > UINTPTR_MAX - size)
            throw std::runtime_error("Invalid native mapped tail");
        const auto last = address + size;
        auto overlaps = [](uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
            return a < d && c < b;
        };
        const char *result = nullptr;
        for (const auto &segment : segments_) {
            if ((segment.p_flags & (PF_R | PF_W | PF_X)) != (PF_R | PF_X) ||
                segment.p_memsz != segment.p_filesz ||
                segment.p_offset % page_size != segment.p_vaddr % page_size)
                continue;
            const uint64_t end = segment.p_vaddr + segment.p_memsz;
            if (end > UINT64_MAX - (page_size - 1))
                continue;
            const auto rounded = (end + page_size - 1) & ~(uint64_t(page_size) - 1);
            if (address < end || last > rounded || address < segment.p_vaddr)
                continue;
            const auto delta = address - segment.p_vaddr;
            if (segment.p_offset > bytes_.size() || delta > bytes_.size() - segment.p_offset ||
                size > bytes_.size() - segment.p_offset - delta)
                continue;
            const auto file = segment.p_offset + delta;
            const auto file_end = file + size;
            bool occupied =
                overlaps(file, file_end, 0, sizeof(Elf64_Ehdr)) ||
                overlaps(file, file_end, header_.e_phoff,
                         header_.e_phoff + size_t(header_.e_phnum) * sizeof(Elf64_Phdr)) ||
                overlaps(file, file_end, header_.e_shoff,
                         header_.e_shoff + size_t(header_.e_shnum) * sizeof(Elf64_Shdr));
            for (const auto &section : sections_) {
                if ((section.sh_flags & SHF_ALLOC) &&
                    overlaps(address, last, section.sh_addr, section.sh_addr + section.sh_size))
                    occupied = true;
                if (section.sh_type != SHT_NOBITS && overlaps(file, file_end, section.sh_offset,
                                                              section.sh_offset + section.sh_size))
                    occupied = true;
            }
            for (const auto &other : segments_) {
                if (&other == &segment || !other.p_memsz)
                    continue;
                const auto other_end = other.p_vaddr + other.p_memsz;
                if (other_end > UINT64_MAX - (page_size - 1)) {
                    occupied = true;
                    continue;
                }
                const auto begin = other.p_vaddr & ~(uint64_t(page_size) - 1);
                const auto finish = (other_end + page_size - 1) & ~(uint64_t(page_size) - 1);
                if (overlaps(address, last, begin, finish))
                    occupied = true;
            }
            const auto *data = bytes_.data() + file;
            if (occupied || !std::all_of(data, data + size, [](char byte) { return byte == 0; }))
                continue;
            if (result)
                throw std::runtime_error("Ambiguous native mapped tail");
            result = data;
        }
        if (!result)
            throw std::runtime_error("Native mapped tail is unavailable");
        return result;
    }
    std::optional<uintptr_t> find_unique(const std::vector<uint8_t> &pattern) const {
        const auto text = section(".text");
        if (!(text.sh_flags & SHF_EXECINSTR) || (text.sh_flags & SHF_WRITE) || pattern.empty())
            throw std::runtime_error("Invalid native code section");
        const auto data = at(text.sh_addr, text.sh_size, true);
        std::optional<uintptr_t> result;
        for (size_t i = 0; i <= text.sh_size && pattern.size() <= text.sh_size - i; i += 4)
            if (!std::memcmp(data + i, pattern.data(), pattern.size())) {
                if (result)
                    throw std::runtime_error("Ambiguous native instruction signature");
                result = text.sh_addr + i;
            }
        return result;
    }
    uintptr_t unique(const std::vector<uint8_t> &pattern) const {
        const auto result = find_unique(pattern);
        if (!result)
            throw std::runtime_error("Native instruction signature is unavailable");
        return *result;
    }
    uintptr_t padding(bool after_plt, size_t size) const {
        const auto plt = after_plt ? section(".plt") : Elf64_Shdr{};
        for (const auto &segment : segments_) {
            if (segment.p_flags != (PF_R | PF_X) || segment.p_memsz != segment.p_filesz)
                continue;
            const auto end = segment.p_vaddr + segment.p_filesz;
            if (end > UINT64_MAX - 4096)
                continue;
            if (after_plt && (plt.sh_addr < segment.p_vaddr || plt.sh_addr + plt.sh_size > end))
                continue;
            const auto first = after_plt ? plt.sh_addr + plt.sh_size : end;
            const auto start = (first + 64 + 15) & ~uintptr_t(15);
            const auto last = (end + 4095) & ~uintptr_t(4095);
            if (start > last || size > last - start)
                continue;
            bool occupied = false;
            for (const auto &section : sections_)
                if ((section.sh_flags & SHF_ALLOC) && section.sh_size &&
                    section.sh_addr < start + size && start < section.sh_addr + section.sh_size)
                    occupied = true;
            for (const auto &other : segments_)
                if (&other != &segment && other.p_vaddr < start + size &&
                    start < other.p_vaddr + other.p_memsz)
                    occupied = true;
            const auto delta = start - segment.p_vaddr;
            if (segment.p_offset > bytes_.size() || delta > bytes_.size() - segment.p_offset ||
                size > bytes_.size() - segment.p_offset - delta)
                continue;
            const auto data = bytes(segment.p_offset + delta, size);
            if (!occupied && std::all_of(data, data + size, [](char byte) { return !byte; }))
                return start;
        }
        throw std::runtime_error("No safe native instruction padding");
    }
};
} // namespace ct::steamvr
