#pragma once
#include "code.h"
#include <dlfcn.h>
#include <link.h>

namespace ct::steamvr {
class Module {
  public:
    struct Range {
        uintptr_t begin, end;
        bool executable;
    };

  private:
    std::vector<Range> ranges_;

  public:
    explicit Module(const void *address) {
        struct Scan {
            std::vector<Range> ranges;
            bool found = false, failed = false;
        } scan;
        dl_iterate_phdr(
            [](dl_phdr_info *info, size_t, void *context) {
                auto &scan = *static_cast<Scan *>(context);
                if (info->dlpi_name && *info->dlpi_name)
                    return 0;
                if (scan.found) {
                    scan.failed = true;
                    return 1;
                }
                scan.found = true;
                try {
                    const uintptr_t base = info->dlpi_addr;
                    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
                        const auto &s = info->dlpi_phdr[i];
                        if (s.p_type != PT_LOAD || !(s.p_flags & PF_R))
                            continue;
                        if (s.p_vaddr > UINTPTR_MAX - base ||
                            s.p_memsz > UINTPTR_MAX - base - s.p_vaddr) {
                            scan.failed = true;
                            return 1;
                        }
                        scan.ranges.push_back({base + s.p_vaddr, base + s.p_vaddr + s.p_memsz,
                                               bool(s.p_flags & PF_X) && !(s.p_flags & PF_W)});
                    }
                } catch (...) {
                    scan.failed = true;
                    return 1;
                }
                return 0;
            },
            &scan);
        ranges_ = std::move(scan.ranges);
        if (scan.failed || !scan.found || !contains(address, 1))
            throw std::runtime_error("Native interface mapping is unavailable");
    }
    static Module loaded(const void *address, const char *expected_path) {
        if (!address || !expected_path || expected_path[0] != '/')
            throw std::runtime_error("Invalid native module identity");
        struct Scan {
            uintptr_t address;
            const char *path;
            size_t matches = 0;
            bool failed = false;
            std::vector<Range> ranges;
        } scan{reinterpret_cast<uintptr_t>(address), expected_path, 0, false, {}};
        dl_iterate_phdr(
            [](dl_phdr_info *info, size_t, void *context) {
                auto &scan = *static_cast<Scan *>(context);
                try {
                    std::vector<Range> ranges, all_ranges;
                    bool matches = false, invalid = false;
                    const uintptr_t base = info->dlpi_addr;
                    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
                        const auto &s = info->dlpi_phdr[i];
                        if (s.p_type != PT_LOAD || !s.p_memsz)
                            continue;
                        if (s.p_vaddr > UINTPTR_MAX - base ||
                            s.p_memsz > UINTPTR_MAX - base - s.p_vaddr) {
                            invalid = true;
                            continue;
                        }
                        Range range{base + s.p_vaddr, base + s.p_vaddr + s.p_memsz,
                                    bool(s.p_flags & PF_X) && !(s.p_flags & PF_W)};
                        if ((s.p_flags & PF_R) && scan.address >= range.begin &&
                            scan.address < range.end)
                            matches = true;
                        for (const auto &other : all_ranges)
                            if (range.begin < other.end && other.begin < range.end)
                                invalid = true;
                        all_ranges.push_back(range);
                        if (s.p_flags & PF_R)
                            ranges.push_back(range);
                    }
                    if (matches) {
                        ++scan.matches;
                        if (invalid || !info->dlpi_name || std::strcmp(info->dlpi_name, scan.path))
                            scan.failed = true;
                        else
                            scan.ranges = std::move(ranges);
                    }
                } catch (...) {
                    scan.failed = true;
                    return 1;
                }
                return 0;
            },
            &scan);
        if (scan.failed || scan.matches != 1)
            throw std::runtime_error("Native shared module is unavailable");
        return Module(std::move(scan.ranges));
    }
    explicit Module(std::vector<Range> ranges) : ranges_(std::move(ranges)) {
        if (ranges_.empty())
            throw std::runtime_error("Native interface mapping is unavailable");
        for (const auto &range : ranges_)
            if (range.end < range.begin)
                throw std::runtime_error("Invalid native interface mapping");
    }
    bool contains(const void *address, size_t size, bool executable = false) const {
        const auto start = reinterpret_cast<uintptr_t>(address);
        size_t matches = 0;
        bool allowed = false;
        for (const auto &range : ranges_)
            if (start >= range.begin && start <= range.end && size <= range.end - start) {
                ++matches;
                allowed = !executable || range.executable;
            }
        return matches == 1 && allowed;
    }
    bool type(const uintptr_t *table, size_t count, const char *name, intptr_t offset = 0) const {
        const auto address = reinterpret_cast<uintptr_t>(table);
        if (!name || address % alignof(uintptr_t) || address < 16 ||
            count > (SIZE_MAX / sizeof(uintptr_t)) - 2 ||
            !contains(reinterpret_cast<const void *>(address - 16), (count + 2) * 8) ||
            table[-2] != uintptr_t(offset) || table[-1] % alignof(uintptr_t) ||
            !contains(reinterpret_cast<const void *>(table[-1]), 16))
            return false;
        const auto *info = reinterpret_cast<const uintptr_t *>(table[-1]);
        const auto *text = reinterpret_cast<const char *>(info[1]);
        const auto length = std::strlen(name);
        return contains(text, length + 1) && !std::memcmp(text, name, length + 1);
    }
    bool method(uintptr_t address, size_t size, const char *signature) const {
        if (!signature || !size || size % 4 || size > 16384 || address % 4 ||
            !contains(reinterpret_cast<const void *>(address), size, true))
            return false;
        return code_signature(reinterpret_cast<const void *>(address), size) == signature;
    }
};
} // namespace ct::steamvr
