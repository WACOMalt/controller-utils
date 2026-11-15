#pragma once
#include "module.h"
#include "queue_abi.h"
#include <array>

namespace ct::steamvr {
template <size_t N>
std::array<uintptr_t, N> read_functions(const void *table, const Module &module) {
    std::array<uintptr_t, N> functions{};
    if (!table || reinterpret_cast<uintptr_t>(table) % alignof(uintptr_t) ||
        !module.contains(table, sizeof(functions)))
        throw std::runtime_error("OpenVR function table is unavailable");
    std::memcpy(functions.data(), table, sizeof(functions));
    for (const auto function : functions)
        if (!function || !module.contains(reinterpret_cast<const void *>(function), 1, true))
            throw std::runtime_error("OpenVR function target is unavailable");
    return functions;
}

class QueueApi final : public BlockQueue {
    struct Functions {
        int (*create)(uint64_t *, char *, uint32_t, uint32_t, uint32_t, uint32_t);
        int (*connect)(uint64_t *, char *);
        int (*destroy)(uint64_t);
        int (*acquire_write)(uint64_t, uint64_t *, void **);
        int (*release_write)(uint64_t, uint64_t);
        int (*wait_read)(uint64_t, uint64_t *, void **, int, uint32_t);
        int (*acquire_read)(uint64_t, uint64_t *, void **, int);
        int (*release_read)(uint64_t, uint64_t);
        int (*has_reader)(uint64_t, bool *);
    } functions_{};

  public:
    explicit QueueApi(const void *table, const Module &module) {
        const auto functions = read_functions<9>(table, module);
        static_assert(sizeof(functions_) == sizeof(functions));
        std::memcpy(&functions_, functions.data(), sizeof(functions_));
    }
    int Create(uint64_t *handle, const char *path, uint32_t size, uint32_t header, uint32_t count,
               uint32_t flags) override {
        return functions_.create(handle, const_cast<char *>(path), size, header, count, flags);
    }
    int Connect(uint64_t *handle, const char *path) override {
        return functions_.connect(handle, const_cast<char *>(path));
    }
    int Destroy(uint64_t handle) override {
        return functions_.destroy(handle);
    }
    int AcquireWriteOnlyBlock(uint64_t handle, uint64_t *block, void **data) override {
        return functions_.acquire_write(handle, block, data);
    }
    int ReleaseWriteOnlyBlock(uint64_t handle, uint64_t block) override {
        return functions_.release_write(handle, block);
    }
    int WaitAndAcquireReadOnlyBlock(uint64_t handle, uint64_t *block, void **data, int kind,
                                    uint32_t timeout) override {
        return functions_.wait_read(handle, block, data, kind, timeout);
    }
    int AcquireReadOnlyBlock(uint64_t handle, uint64_t *block, void **data, int kind) override {
        return functions_.acquire_read(handle, block, data, kind);
    }
    int ReleaseReadOnlyBlock(uint64_t handle, uint64_t block) override {
        return functions_.release_read(handle, block);
    }
    int QueueHasReader(uint64_t handle, bool *present) override {
        return functions_.has_reader(handle, present);
    }
};
} // namespace ct::steamvr
