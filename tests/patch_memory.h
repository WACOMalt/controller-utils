#pragma once
#include "helper/patch.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace ct::test {
class PatchMemory final : public helper::PatchMemory {
    helper::Bytes &region(uintptr_t address, size_t size, uintptr_t &offset) {
        for (auto &[base, bytes] : regions)
            if (address >= base && address - base <= bytes.size() &&
                size <= bytes.size() - (address - base)) {
                offset = address - base;
                return bytes;
            }
        throw std::runtime_error("test memory address is outside registered regions");
    }
    void write(uintptr_t address, const helper::Bytes &value) {
        addresses.push_back(address);
        ++writes;
        const bool failed = writes == fail_at;
        if (failed && !fail_after)
            throw std::runtime_error("injected patch write failure");
        uintptr_t offset;
        auto &bytes = region(address, value.size(), offset);
        std::copy(value.begin(), value.end(), bytes.begin() + offset);
        if (failed)
            throw std::runtime_error("injected patch write failure");
    }

  public:
    std::map<uintptr_t, helper::Bytes> regions;
    std::vector<uintptr_t> addresses;
    size_t writes = 0, fail_at = 0;
    bool fail_after = false;
    helper::Bytes read(uintptr_t address, size_t size) override {
        uintptr_t offset;
        auto &bytes = region(address, size, offset);
        return {bytes.begin() + offset, bytes.begin() + offset + size};
    }
    void code(uintptr_t address, const helper::Bytes &value) override {
        write(address, value);
    }
    void data(uintptr_t address, const helper::Bytes &value) override {
        write(address, value);
    }
};
} // namespace ct::test
