#include "../driver/native_queue_startup.h"
#include <cassert>
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <typeinfo>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

class Queue final : public ct::steamvr::BlockQueue {
  public:
    std::map<std::string, uint64_t> handles;
    unsigned creates = 0, connections = 0;
    int failure = 0;
    std::mutex lock;
    int Create(uint64_t *handle, const char *path, uint32_t, uint32_t, uint32_t,
               uint32_t) override {
        std::lock_guard<std::mutex> guard(lock);
        ++creates;
        if (failure)
            return failure;
        if (handles.count(path))
            return 1;
        *handle = handles[path] = handles.size() + 1;
        return 0;
    }
    int Connect(uint64_t *handle, const char *path) override {
        std::lock_guard<std::mutex> guard(lock);
        ++connections;
        if (failure)
            return failure;
        if (!handles.count(path))
            return 2;
        *handle = handles[path];
        return 0;
    }
    int Destroy(uint64_t) override {
        assert(false);
        return 0;
    }
    int AcquireWriteOnlyBlock(uint64_t, uint64_t *, void **) override {
        return 17;
    }
    int ReleaseWriteOnlyBlock(uint64_t, uint64_t) override {
        return 18;
    }
    int WaitAndAcquireReadOnlyBlock(uint64_t, uint64_t *, void **, int, uint32_t) override {
        return 19;
    }
    int AcquireReadOnlyBlock(uint64_t, uint64_t *, void **, int) override {
        return 20;
    }
    int ReleaseReadOnlyBlock(uint64_t, uint64_t) override {
        return 21;
    }
    int QueueHasReader(uint64_t, bool *) override {
        return 22;
    }
};

__attribute__((noinline)) int stock_create(ct::steamvr::BlockQueue *queue, uint64_t *handle,
                                           const char *path, uint32_t size, uint32_t header = 512,
                                           uint32_t count = 4, uint32_t flags = 0) {
    return queue->Create(handle, path, size, header, count, flags);
}
__attribute__((noinline)) void untouched(ct::steamvr::BlockQueue *queue) {
    assert(typeid(*queue) == typeid(Queue));
    assert(queue->AcquireWriteOnlyBlock(0, nullptr, nullptr) == 17);
    assert(queue->ReleaseWriteOnlyBlock(0, 0) == 18);
    assert(queue->WaitAndAcquireReadOnlyBlock(0, nullptr, nullptr, 0, 0) == 19);
    assert(queue->AcquireReadOnlyBlock(0, nullptr, nullptr, 0) == 20);
    assert(queue->ReleaseReadOnlyBlock(0, 0) == 21);
    assert(queue->QueueHasReader(0, nullptr) == 22);
}

int main() {
    using namespace ct::queue_startup;
    auto initialize = [](ct::steamvr::BlockQueue *queue) {
        return ct::queue_startup::initialize_with(
            queue,
            [](ct::steamvr::BlockQueue *q, const uintptr_t *) {
                return typeid(*q) == typeid(Queue);
            },
            [] { return true; });
    };
    assert(!initialize(nullptr));

    for (const auto path : {"/xrservice/controller/data", "/xrservice/controller/event"}) {
        const auto child = fork();
        assert(child >= 0);
        if (!child) {
            Queue existing;
            existing.handles[path] = 42;
            assert(!initialize(&existing));
            uint64_t handle = 999;
            const auto size = controller_index(path) == 0 ? 48 : 24592;
            assert(stock_create(&existing, &handle, path, size) == 1 && handle == 999);
            _exit(0);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
    }

    Queue fake;
    assert(initialize(&fake));
    assert(fake.handles.size() == 2 && fake.creates == 2);
    const auto data = fake.handles.at("/xrservice/controller/data");
    const auto event = fake.handles.at("/xrservice/controller/event");
    uint64_t handle = 0;
    assert(stock_create(&fake, &handle, "/xrservice/controller/data", 48) == 0 && handle == data);
    assert(stock_create(&fake, &handle, "/xrservice/controller/event", 24592) == 0 &&
           handle == event);
    assert(initialize(&fake));
    assert(fake.handles.size() == 2);
    auto connect_later = [&] {
        for (unsigned i = 0; i < 100; ++i) {
            uint64_t id = 0;
            assert(stock_create(&fake, &id, "/xrservice/controller/data", 48) == 0 && id == data);
            assert(stock_create(&fake, &id, "/xrservice/controller/event", 24592) == 0 &&
                   id == event);
        }
    };
    std::thread left(connect_later), right(connect_later);
    left.join();
    right.join();
    for (auto values : {std::array<uint32_t, 4>{49, 512, 4, 0},
                        {48, 513, 4, 0},
                        {48, 512, 5, 0},
                        {48, 512, 4, 1}}) {
        handle = 999;
        assert(stock_create(&fake, &handle, "/xrservice/controller/data", values[0], values[1],
                            values[2], values[3]) == 1 &&
               handle == 999);
    }
    assert(stock_create(&fake, &handle, "/another/queue", 16) == 0);
    handle = 999;
    assert(stock_create(&fake, &handle, "/another/queue", 16) == 1 && handle == 999);
    fake.failure = 7;
    assert(stock_create(&fake, &handle, "/xrservice/controller/data", 48) == 7);
    assert(!initialize(&fake));
    fake.failure = 0;
    untouched(&fake);
    Queue other;
    assert(!initialize(&other));
    fake.handles["/xrservice/controller/data"] = 100;
    handle = 999;
    assert(stock_create(&fake, &handle, "/xrservice/controller/data", 48) == 1 && handle == 999);
    assert(!initialize(&fake));
    std::puts("native queue startup: cold creation, later stock attachment, exact schema, retries "
              "and rejection of unknown queue sizes and ABI passed");
}
