#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace ct {
struct MemoryMapping {
    uintptr_t first, last, offset;
    dev_t device;
    uint64_t inode;
    bool readable, writable, executable;
    std::string path;
    bool private_mapping = true;
    bool contains(uintptr_t address, size_t size) const;
};
std::vector<MemoryMapping> process_mappings(const std::string &text);
} // namespace ct
