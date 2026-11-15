#pragma once
#include "steamvr/queue_abi.h"
#include "steamvr/queue_validation.h"
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <functional>
#include <mutex>

namespace ct::queue_startup {
using Create = int (*)(ct::steamvr::BlockQueue *, uint64_t *, const char *, uint32_t, uint32_t,
                       uint32_t, uint32_t);
inline ct::steamvr::BlockQueue *manager;
inline uintptr_t original[11], replacement[11];
inline std::mutex install_lock;
inline uint64_t controller_handles[2]{};

inline int controller_index(const char *path) {
    if (path && !std::strcmp(path, "/xrservice/controller/data"))
        return 0;
    if (path && !std::strcmp(path, "/xrservice/controller/event"))
        return 1;
    return -1;
}

inline bool known_queue(const char *path, uint64_t handle) {
    const int index = controller_index(path);
    std::lock_guard<std::mutex> lock(install_lock);
    return index >= 0 && handle && controller_handles[index] == handle;
}

inline bool controller_queue(const char *path, uint32_t size, uint32_t header, uint32_t count,
                             uint32_t flags) {
    return path && header == 512 && count == 4 && flags == 0 &&
           ((!std::strcmp(path, "/xrservice/controller/data") && size == 48) ||
            (!std::strcmp(path, "/xrservice/controller/event") && size == 24592));
}

inline int create(ct::steamvr::BlockQueue *self, uint64_t *handle, const char *path, uint32_t size,
                  uint32_t header, uint32_t count, uint32_t flags) {
    const int result =
        reinterpret_cast<Create>(original[2])(self, handle, path, size, header, count, flags);
    const int index = controller_index(path);
    const bool matches = controller_queue(path, size, header, count, flags);
    if (!result && self == manager && index >= 0) {
        std::lock_guard<std::mutex> lock(install_lock);
        controller_handles[index] = matches ? *handle : 0;
    }
    // Valve's first controller must attach to queues created before its activation.
    if (result == 1 && self == manager && matches) {
        uint64_t existing = 0;
        const int connected =
            reinterpret_cast<int (*)(ct::steamvr::BlockQueue *, uint64_t *, const char *)>(
                original[3])(self, &existing, path);
        if (!connected && known_queue(path, existing)) {
            *handle = existing;
            return 0;
        }
    }
    return result;
}

inline bool validate_reviewed_queue(ct::steamvr::BlockQueue *, const uintptr_t *table) {
    try {
        return ct::steamvr::validate_queue_layout(table, ct::steamvr::Module(table),
                                                  ct::steamvr::ServerQueueLayout);
    } catch (...) {
        return false;
    }
}
using Validator = std::function<bool(ct::steamvr::BlockQueue *, const uintptr_t *)>;
using Retainer = bool (*)();
inline bool retain_hook_module() {
    Dl_info own{};
    return dladdr(reinterpret_cast<void *>(create), &own) && own.dli_fname &&
           dlopen(own.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
}
inline bool install(ct::steamvr::BlockQueue *queue, const Validator &validate, Retainer retain) {
    if (!queue)
        return false;
    std::lock_guard<std::mutex> lock(install_lock);
    auto **object = reinterpret_cast<uintptr_t **>(queue);
    auto *table = __atomic_load_n(object, __ATOMIC_ACQUIRE);
    if (manager)
        return manager == queue && table == replacement + 2;
    if (!validate || !retain || !validate(queue, table) || !retain())
        return false;
    std::memcpy(original, table - 2, sizeof(original));
    std::memcpy(replacement, original, sizeof(replacement));
    replacement[2] = reinterpret_cast<uintptr_t>(create);
    manager = queue;
    if (!__atomic_compare_exchange_n(object, &table, replacement + 2, false, __ATOMIC_RELEASE,
                                     __ATOMIC_ACQUIRE)) {
        manager = nullptr;
        return false;
    }
    return true;
}

inline bool initialize_with(ct::steamvr::BlockQueue *queue, const Validator &validate,
                            Retainer retain) {
    if (!install(queue, validate, retain))
        return false;
    for (const auto *path : {"/xrservice/controller/data", "/xrservice/controller/event"}) {
        uint64_t handle = 0;
        int result = queue->Connect(&handle, path);
        if (result == 2)
            result =
                create(queue, &handle, path,
                       !std::strcmp(path, "/xrservice/controller/data") ? 48 : 24592, 512, 4, 0);
        // BlockQueue_005 exposes no capacity query. Only accept witnessed Create sizes.
        if (result || !known_queue(path, handle))
            return false;
    }
    // These are shared process-wide owners, Destroy would also invalidate Valve's handle.
    return true;
}
inline bool initialize(ct::steamvr::BlockQueue *queue) {
    return initialize_with(queue, validate_reviewed_queue, retain_hook_module);
}
} // namespace ct::queue_startup
