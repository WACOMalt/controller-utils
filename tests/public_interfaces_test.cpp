#include "steamvr/queue_api.h"
#include <cassert>
#include <iostream>

namespace {
uint64_t calls = 0;
int payload = 42;
int create(uint64_t *handle, char *path, uint32_t size, uint32_t header, uint32_t count,
           uint32_t flags) {
    assert(!std::strcmp(path, "/test") && size == 48 && header == 512 && count == 4 && flags == 1);
    *handle = 11;
    calls |= 1;
    return 0;
}
int connect(uint64_t *handle, char *path) {
    assert(!std::strcmp(path, "/test"));
    *handle = 12;
    calls |= 2;
    return 2;
}
int destroy(uint64_t handle) {
    assert(handle == 11);
    calls |= 4;
    return 4;
}
int acquire_write(uint64_t handle, uint64_t *block, void **data) {
    assert(handle == 11);
    *block = 13;
    *data = &payload;
    calls |= 8;
    return 0;
}
int release_write(uint64_t handle, uint64_t block) {
    assert(handle == 11 && block == 13);
    calls |= 16;
    return 5;
}
int wait_read(uint64_t handle, uint64_t *block, void **data, int kind, uint32_t timeout) {
    assert(handle == 11 && kind == 1 && timeout == 100);
    *block = 14;
    *data = &payload;
    calls |= 32;
    return 0;
}
int acquire_read(uint64_t handle, uint64_t *block, void **data, int kind) {
    assert(handle == 11 && kind == 2);
    *block = 14;
    *data = &payload;
    calls |= 64;
    return 0;
}
int release_read(uint64_t handle, uint64_t block) {
    assert(handle == 11 && block == 14);
    calls |= 128;
    return 6;
}
int has_reader(uint64_t handle, bool *present) {
    assert(handle == 11);
    *present = true;
    calls |= 256;
    return 0;
}
template <class F> void rejects(F function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main() {
    using ct::steamvr::Module;
    std::array<uintptr_t, 9> table{
        reinterpret_cast<uintptr_t>(create),        reinterpret_cast<uintptr_t>(connect),
        reinterpret_cast<uintptr_t>(destroy),       reinterpret_cast<uintptr_t>(acquire_write),
        reinterpret_cast<uintptr_t>(release_write), reinterpret_cast<uintptr_t>(wait_read),
        reinterpret_cast<uintptr_t>(acquire_read),  reinterpret_cast<uintptr_t>(release_read),
        reinterpret_cast<uintptr_t>(has_reader)};
    const auto original = table;
    const auto address = reinterpret_cast<uintptr_t>(table.data());
    std::vector<Module::Range> ranges{{address, address + sizeof(table), false}};
    for (const auto function : table)
        ranges.push_back({function, function + 1, true});
    Module module(ranges);
    ct::steamvr::QueueApi queue(table.data(), module);
    table.fill(0);
    uint64_t handle = 0, block = 0;
    void *data = nullptr;
    bool present = false;
    assert(queue.Create(&handle, "/test", 48, 512, 4, 1) == 0 && handle == 11);
    assert(queue.Connect(&handle, "/test") == 2 && handle == 12);
    assert(queue.Destroy(11) == 4);
    assert(queue.AcquireWriteOnlyBlock(11, &block, &data) == 0 && block == 13 && data == &payload);
    assert(queue.ReleaseWriteOnlyBlock(11, block) == 5);
    assert(queue.WaitAndAcquireReadOnlyBlock(11, &block, &data, 1, 100) == 0 && block == 14 &&
           data == &payload);
    assert(queue.AcquireReadOnlyBlock(11, &block, &data, 2) == 0 && block == 14 &&
           data == &payload);
    assert(queue.ReleaseReadOnlyBlock(11, block) == 6);
    assert(queue.QueueHasReader(11, &present) == 0 && present);
    assert(calls == 511);
    rejects([&] { ct::steamvr::QueueApi invalid(table.data(), module); });
    table = original;
    for (size_t i = 0; i < table.size(); ++i) {
        table[i] = address;
        rejects([&] { ct::steamvr::QueueApi invalid(table.data(), module); });
        table = original;
        auto missing = ranges;
        missing.erase(missing.begin() + i + 1);
        rejects([&] { ct::steamvr::QueueApi invalid(table.data(), Module(missing)); });
    }
    rejects([&] { ct::steamvr::read_functions<9>(nullptr, module); });
    rejects([&] { ct::steamvr::read_functions<9>(table.data() + 1, module); });
    --ranges[0].end;
    rejects([&] { ct::steamvr::QueueApi invalid(table.data(), Module(ranges)); });
    assert(calls == 511);
    std::cout << "public interfaces: ABI arguments, results, copied targets and bounded rejection "
                 "passed\n";
}
