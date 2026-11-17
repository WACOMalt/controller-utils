#define CT_ISOLATION_TEST
#include "../driver/native_tracker.cpp"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>
#include <chrono>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

using namespace ct::tracker;
static void *stock_manager = reinterpret_cast<void *>(0x1110);
static void *owned_manager = reinterpret_cast<void *>(0x2220);
static Control state{};
static std::atomic<unsigned> destroys{}, private_calls{}, stock_calls{}, sequence{};
static std::atomic<bool> blocked{}, release_call{}, identity_valid{true};
static int failure;
static bool native_started;
static bool native_settings_loaded;
static unsigned bootstrap_preset;
static bool fail_thread_create;
static std::atomic<bool> pause_cleanup{}, cleanup_waiting{}, release_cleanup{};
static std::atomic<bool> block_create{}, create_waiting{}, release_create{};
static std::atomic<bool> block_admission{}, admission_waiting{}, release_admission{};
static std::atomic<unsigned> workers_created{};
extern "C" int __real_pthread_create(unsigned long *, const void *, void *(*)(void *), void *);
extern "C" int __wrap_pthread_create(unsigned long *thread, const void *attributes,
                                     void *(*call)(void *), void *argument) {
    if (call != ct::tracker::worker)
        return __real_pthread_create(thread, attributes, call, argument);
    ++workers_created;
    if (block_create) {
        create_waiting = true;
        while (!release_create)
            std::this_thread::yield();
    }
    return fail_thread_create ? 11 : __real_pthread_create(thread, attributes, call, argument);
}
namespace ct::tracker {
void test_worker_admission_boundary() {
    if (block_admission) {
        admission_waiting = true;
        while (!release_admission)
            std::this_thread::yield();
    }
}
void test_cleanup_boundary() {
    if (pause_cleanup) {
        cleanup_waiting = true;
        while (!release_cleanup)
            std::this_thread::yield();
    }
}
bool test_identity() {
    return identity_valid;
}
void test_construct(Wrapper target, void *) {
    *target = owned_manager;
    native_started = false;
    native_settings_loaded = false;
    bootstrap_preset = 0;
    if (failure == 1)
        throw 42;
}
void test_initialize_settings(Wrapper target) {
    assert(*target == owned_manager);
    assert(native_started);
    if (failure == 5)
        throw 46;
    native_settings_loaded = true;
    bootstrap_preset = 1;
}
void test_start(Wrapper) {
    if (failure == 2)
        throw 43;
    native_started = true;
}
void test_configure(Wrapper) {
    bootstrap_preset = 3;
    if (failure == 3)
        throw 44;
}
void test_destroy(Wrapper target) {
    assert(*target == owned_manager);
    ++destroys;
    if (failure == 4)
        throw 45;
    *target = nullptr;
}
} // namespace ct::tracker

static void observed(Wrapper target, u32 id) {
    assert(id == 16 || target == &stock_manager);
    if (target == &stock_manager)
        ++stock_calls;
    else {
        assert(*target == owned_manager);
        ++private_calls;
    }
}
static void frame_next(Wrapper target, void *p, void *h, void *acc, u32 a, void *aux, u32 b, u32 c,
                       void *extra1, void *extra2) {
    assert(p == reinterpret_cast<void *>(1) && h == reinterpret_cast<void *>(2));
    assert(acc == reinterpret_cast<void *>(3) && a == 4 && aux == reinterpret_cast<void *>(5));
    assert(b == 6 && c == 7 && extra1 == reinterpret_cast<void *>(8) &&
           extra2 == reinterpret_cast<void *>(9));
    if (target == &stock_manager)
        sequence = 1;
    else {
        if (!native_settings_loaded) {
            native_settings_loaded = true;
            bootstrap_preset = 1;
        }
        assert(bootstrap_preset == 3);
        assert(sequence == 1);
        sequence = 2;
    }
    observed(target, 16);
}
static u32 imu_next(Wrapper target, void *p, void *measured, u32 id, void *output, float scalar) {
    assert(p == reinterpret_cast<void *>(1) && measured == reinterpret_cast<void *>(2));
    assert(output == reinterpret_cast<void *>(4) && scalar == .25f);
    observed(target, id);
    if (target != &stock_manager && blocked)
        while (!release_call)
            std::this_thread::yield();
    return 123;
}
static void connection_next(Wrapper target, u32 id, u32 connected) {
    assert(connected <= 1);
    observed(target, id);
}
static NativeString profile{"{\"model\":\"gamepad\"}", 19, {0, 0}};
static void config_next(Wrapper target, void *json, u32 id) {
    const auto &text = *static_cast<const NativeString *>(json);
    assert(text.length == profile.length && !std::memcmp(text.data, profile.data, text.length));
    observed(target, id);
}
static std::atomic<unsigned> prechecking{};
static u32 precheck_next(Wrapper target, u32 id) {
    assert(prechecking.fetch_add(1) == 0);
    struct Done {
        ~Done() {
            --prechecking;
        }
    } done;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    if (id == 99)
        throw 99;
    observed(target, id);
    return 1;
}
static double clock_next(Wrapper target, u32 id) {
    observed(target, id);
    return .125;
}
static std::atomic<unsigned> publishing{};
static std::atomic<bool> pause_publish{}, publish_waiting{}, release_publish{};
static void publisher_next(void *, u32 id, void *) {
    assert(publishing.fetch_add(1) == 0);
    struct Done {
        ~Done() {
            --publishing;
        }
    } done;
    if (id == 1 && pause_publish) {
        publish_waiting = true;
        while (!release_publish)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    if (id == 99)
        throw 99;
}
static std::atomic<unsigned> imaging{};
static std::atomic<unsigned> predicting{}, metadata_calls{}, transforming{};
struct Copying {
    std::atomic<unsigned> &active;
    Copying(std::atomic<unsigned> &value, void *manager) : active(value) {
        assert(active.fetch_add(1) == 0);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (manager == reinterpret_cast<void *>(99)) {
            --active;
            throw 99;
        }
    }
    ~Copying() {
        --active;
    }
};
static void predictions_next(void *manager, void *output, double timestamp) {
    assert(output == reinterpret_cast<void *>(2) && timestamp == 123.25);
    Copying copy(predicting, manager);
}
static void metadata_next(void *manager, u32 id) {
    assert(id == 16);
    Copying copy(metadata_calls, manager);
}
static void transform_next(void *manager, void *output) {
    assert(output == reinterpret_cast<void *>(2));
    Copying copy(transforming, manager);
}
static void image_next(void *detector, void *pixels, void *output, void *crop, void *settings,
                       void *history) {
    assert(detector && pixels == reinterpret_cast<void *>(2) &&
           output == reinterpret_cast<void *>(3));
    assert(crop == reinterpret_cast<void *>(4) && settings == reinterpret_cast<void *>(5) &&
           history == reinterpret_cast<void *>(6));
    assert(imaging.fetch_add(1) == 0);
    struct Done {
        ~Done() {
            --imaging;
        }
    } done;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    if (detector == reinterpret_cast<void *>(99))
        throw 99;
}
static void wait_phase(Phase phase) {
    for (unsigned i = 0; i < 2000 && load(&state.phase) != phase; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(load(&state.phase) == phase);
}
static void begin() {
    state.magic = Magic;
    state.version = 1;
    state.command = 1;
    state.generation = 1;
    state.loader_started = 1;
    state.deadline_raw_ns = now() + 2000000000ULL;
    word table[] = {word(frame_next),     word(imu_next),      word(connection_next),
                    word(config_next),    word(precheck_next), word(clock_next),
                    word(publisher_next), word(image_next),    word(predictions_next),
                    word(metadata_next),  word(transform_next)};
    ct_tracker_attach(&state, &stock_manager, table);
}
static void feeds(u32 id) {
    reinterpret_cast<ConnectionCall>(state.entry[Connection])(&stock_manager, id, 1);
    reinterpret_cast<ConfigurationCall>(state.entry[Configuration])(&stock_manager, &profile, id);
    reinterpret_cast<ImuCall>(state.entry[Imu])(&stock_manager, reinterpret_cast<void *>(1),
                                                reinterpret_cast<void *>(2), id,
                                                reinterpret_cast<void *>(4), .25f);
    reinterpret_cast<PrecheckCall>(state.entry[Precheck])(&stock_manager, id);
    reinterpret_cast<ClockCall>(state.entry[Clock])(&stock_manager, id);
}
static void test_routing() {
    begin();
    wait_phase(Ready);
    assert(state.loader_started == 1 && !state.worker_exited);
    feeds(1);
    feeds(2);
    assert(stock_calls == 10 && private_calls == 0);
    feeds(16);
    assert(stock_calls == 10 && private_calls == 5);
    assert(!load(&state.active));
    reinterpret_cast<ClockCall>(state.entry[Clock])(&private_manager, 16);
    assert(!load(&state.active) && private_calls == 6);
    assert(reinterpret_cast<ImuCall>(state.entry[Imu])(reinterpret_cast<Wrapper>(0x3330), nullptr,
                                                       nullptr, 16, nullptr, 0) == 0);
    assert(stock_calls == 10 && private_calls == 6);
    reinterpret_cast<FrameCall>(state.entry[Frame])(
        &stock_manager, reinterpret_cast<void *>(1), reinterpret_cast<void *>(2),
        reinterpret_cast<void *>(3), 4, reinterpret_cast<void *>(5), 6, 7,
        reinterpret_cast<void *>(8), reinterpret_cast<void *>(9));
    assert(sequence == 2 && state.frames == 1 && state.imu == 1);
    store(&state.command, 0u);
    wait_phase(Stopped);
    auto previous = private_calls.load();
    feeds(16);
    feeds(2);
    assert(private_calls == previous && stock_calls == 16 && destroys == 1);
    assert(std::isnan(reinterpret_cast<ClockCall>(state.entry[Clock])(&stock_manager, 16)));
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    wait_phase(Ready);
    auto before = private_calls.load();
    reinterpret_cast<ConnectionCall>(state.entry[Connection])(&stock_manager, 16, 1);
    assert(private_calls == before + 2);
    reinterpret_cast<ConfigurationCall>(state.entry[Configuration])(&stock_manager, &profile, 16);
    assert(private_calls == before + 2);
    store(&state.command, 0u);
    wait_phase(Stopped);
    assert(destroys == 2);
}
static void test_late_config_is_owned_and_replayed() {
    begin();
    wait_phase(Ready);
    store(&state.command, 0u);
    wait_phase(Stopped);
    char text[] = "{\"model\":\"gamepad\"}";
    NativeString late{text, sizeof(text) - 1, {0, 0}};
    reinterpret_cast<ConfigurationCall>(state.entry[Configuration])(&stock_manager, &late, 16);
    std::memset(text, 'x', sizeof(text) - 1);
    assert(!private_calls && !stock_calls);
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    wait_phase(Ready);
    reinterpret_cast<ConnectionCall>(state.entry[Connection])(&stock_manager, 16, 1);
    assert(private_calls == 2);
    NativeString oversized{profile.data, sizeof(cached_config), {0, 0}};
    reinterpret_cast<ConfigurationCall>(state.entry[Configuration])(&stock_manager, &oversized, 16);
    wait_phase(Failed);
    assert(state.error == 15 && !state.active && destroys == 2);
}
static void test_inflight_cleanup() {
    begin();
    wait_phase(Ready);
    blocked = true;
    std::thread caller([] { feeds(16); });
    while (private_calls < 3 || !load(&state.active))
        std::this_thread::yield();
    store(&state.command, 0u);
    wait_phase(Stopping);
    assert(destroys == 0);
    feeds(1);
    assert(stock_calls == 5);
    release_call = true;
    caller.join();
    wait_phase(Stopped);
    assert(destroys == 1);
}
static void test_failure() {
    begin();
    wait_phase(Failed);
    assert(destroys == (failure == 1 ? 0 : 1));
    feeds(16);
    assert(!private_calls && !stock_calls);
    feeds(1);
    assert(stock_calls == 5);
}
static void test_late_callback_drains_after_failure() {
    begin();
    wait_phase(Ready);
    blocked = true;
    std::thread caller([] { feeds(16); });
    while (private_calls < 3 || !load(&state.active))
        std::this_thread::yield();
    store(&state.command, 0u);
    wait_phase(Failed);
    assert(state.error == 14 && !state.command && load(&state.active) == 1);
    assert(load(&state.manager) == word(owned_manager) && !destroys);
    const auto calls = private_calls.load();
    feeds(16);
    assert(private_calls == calls);
    feeds(1);
    assert(stock_calls == 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!destroys && load(&state.active) == 1);
    release_call = true;
    caller.join();
    for (unsigned i = 0; i < 2000 && load(&state.manager); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!state.manager && !state.wrapper && !state.classic && !state.active);
    wait_phase(Failed);
    assert(!state.worker_exited);
    assert(state.phase == Failed && state.error == 14 && destroys == 1);
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    wait_phase(Ready);
    store(&state.command, 0u);
    wait_phase(Stopped);
    assert(destroys == 2);
}
static void test_lost_identity_retains_resources() {
    begin();
    wait_phase(Ready);
    identity_valid = false;
    store(&state.command, 0u);
    wait_phase(Failed);
    assert(state.error == 14 && state.manager == word(owned_manager) && !destroys);
    assert(state.worker_exited == 1);
    identity_valid = true;
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(state.phase == Failed && state.manager == word(owned_manager) && !destroys);
    assert(!state.active);
    feeds(16);
    assert(!private_calls);
}
static void test_destructor_failure_is_terminal() {
    begin();
    wait_phase(Ready);
    failure = 4;
    store(&state.command, 0u);
    wait_phase(Failed);
    assert(state.error == 2 && state.manager == word(owned_manager) && destroys == 1);
    assert(state.worker_exited == 1);
    failure = 0;
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(state.phase == Failed && state.manager == word(owned_manager) && destroys == 1);
    feeds(16);
    assert(!private_calls);
    feeds(1);
    assert(stock_calls == 5);
}
static void test_identity_loss_during_late_drain_is_terminal() {
    begin();
    wait_phase(Ready);
    blocked = true;
    std::thread caller([] { feeds(16); });
    while (private_calls < 3 || !load(&state.active))
        std::this_thread::yield();
    store(&state.command, 0u);
    wait_phase(Failed);
    identity_valid = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    identity_valid = true;
    release_call = true;
    caller.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(load(&state.phase) == Failed && state.error == 14);
    assert(state.worker_exited == 1);
    assert(load(&state.manager) == word(owned_manager) && !state.active && !destroys);
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(load(&state.phase) == Failed && load(&state.manager) == word(owned_manager) &&
           !destroys);
}
static void test_expired_lease() {
    begin();
    wait_phase(Ready);
    store(&state.deadline_raw_ns, now() - 1);
    feeds(16);
    assert(private_calls == 0);
    wait_phase(Stopped);
    assert(destroys == 1);
}
static void test_publishers_share_scratch_safely() {
    begin();
    wait_phase(Ready);
    auto publish = reinterpret_cast<const SharedCallbacks *>(state.callbacks)->publisher;
    std::thread first([&] {
        for (unsigned i = 0; i < 50; ++i)
            publish(stock_manager, 1, nullptr);
    });
    std::thread second([&] {
        for (unsigned i = 0; i < 50; ++i)
            publish(owned_manager, 16, nullptr);
    });
    first.join();
    second.join();
    try {
        publish(owned_manager, 99, nullptr);
        assert(false);
    } catch (int error) {
        assert(error == 99);
    }
    publish(stock_manager, 1, nullptr);
    store(&state.command, 0u);
    wait_phase(Stopped);
}
static void test_readiness_checks_share_scratch_safely() {
    begin();
    wait_phase(Ready);
    auto check = reinterpret_cast<PrecheckCall>(state.entry[Precheck]);
    std::atomic<bool> start{false};
    std::thread first([&] {
        while (!start)
            std::this_thread::yield();
        for (unsigned i = 0; i < 250; ++i)
            assert(check(&stock_manager, 1) == 1);
    });
    std::thread second([&] {
        while (!start)
            std::this_thread::yield();
        for (unsigned i = 0; i < 250; ++i)
            assert(check(&stock_manager, 16) == 1);
    });
    start = true;
    first.join();
    second.join();
    assert(!prechecking && !state.active);
    try {
        check(&stock_manager, 99);
        assert(false);
    } catch (int error) {
        assert(error == 99);
    }
    assert(check(&stock_manager, 16) == 1);
    store(&state.command, 0u);
    wait_phase(Stopped);
}
static void test_contended_publishers_sleep() {
    begin();
    wait_phase(Ready);
    auto publish = reinterpret_cast<const SharedCallbacks *>(state.callbacks)->publisher;
    pause_publish = true;
    std::thread owner([&] { publish(stock_manager, 1, nullptr); });
    while (!publish_waiting)
        std::this_thread::yield();
    std::atomic<bool> waiting{false}, finished{false};
    double used = 0;
    std::thread waiter([&] {
        timespec before{}, after{};
        assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &before));
        waiting = true;
        publish(owned_manager, 16, nullptr);
        assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &after));
        used = double(after.tv_sec - before.tv_sec) + (after.tv_nsec - before.tv_nsec) * 1e-9;
        finished = true;
    });
    while (!waiting)
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!finished && publishing == 1);
    release_publish = true;
    owner.join();
    waiter.join();
    assert(finished && used < .025);
    store(&state.command, 0u);
    wait_phase(Stopped);
}
static void test_image_workers_share_native_queues_safely() {
    begin();
    wait_phase(Ready);
    auto detect = reinterpret_cast<const SharedCallbacks *>(state.callbacks)->image;
    auto call = [&](unsigned detector) {
        detect(reinterpret_cast<void *>(word(detector)), reinterpret_cast<void *>(2),
               reinterpret_cast<void *>(3), reinterpret_cast<void *>(4),
               reinterpret_cast<void *>(5), reinterpret_cast<void *>(6));
    };
    std::atomic<bool> start{false};
    std::thread first([&] {
        while (!start)
            std::this_thread::yield();
        for (unsigned i = 0; i < 250; ++i)
            call(1);
    });
    std::thread second([&] {
        while (!start)
            std::this_thread::yield();
        for (unsigned i = 0; i < 250; ++i)
            call(16);
    });
    start = true;
    first.join();
    second.join();
    assert(!imaging);
    try {
        call(99);
        assert(false);
    } catch (int error) {
        assert(error == 99);
    }
    call(1);
    store(&state.command, 0u);
    wait_phase(Stopped);
    call(1);
}
static void test_remaining_pose_scratch_is_serialized() {
    begin();
    wait_phase(Ready);
    const auto *callbacks = reinterpret_cast<const SharedCallbacks *>(state.callbacks);
    auto call = [&](word manager) {
        callbacks->predictions(reinterpret_cast<void *>(manager), reinterpret_cast<void *>(2),
                               123.25);
        callbacks->metadata(reinterpret_cast<void *>(manager), 16);
        callbacks->transform(reinterpret_cast<void *>(manager), reinterpret_cast<void *>(2));
    };
    std::atomic<bool> start{false};
    std::thread first([&] {
        while (!start)
            std::this_thread::yield();
        for (unsigned i = 0; i < 250; ++i)
            call(1);
    });
    std::thread second([&] {
        while (!start)
            std::this_thread::yield();
        for (unsigned i = 0; i < 250; ++i)
            call(16);
    });
    start = true;
    first.join();
    second.join();
    assert(!predicting && !metadata_calls && !transforming);
    for (unsigned kind = 0; kind < 3; ++kind) {
        try {
            if (kind == 0)
                callbacks->predictions(reinterpret_cast<void *>(99), reinterpret_cast<void *>(2),
                                       123.25);
            else if (kind == 1)
                callbacks->metadata(reinterpret_cast<void *>(99), 16);
            else
                callbacks->transform(reinterpret_cast<void *>(99), reinterpret_cast<void *>(2));
            assert(false);
        } catch (int error) {
            assert(error == 99);
        }
        call(1);
    }
    store(&state.command, 0u);
    wait_phase(Stopped);
    call(1);
}
static void test_cleanup_publishes_final_state_last() {
    begin();
    wait_phase(Ready);
    blocked = true;
    std::thread caller([] { feeds(16); });
    while (private_calls < 3 || !load(&state.active))
        std::this_thread::yield();
    store(&state.command, 0u);
    wait_phase(Failed);
    pause_cleanup = true;
    release_call = true;
    caller.join();
    while (!cleanup_waiting)
        std::this_thread::yield();
    assert(!state.manager && !state.wrapper && !state.classic && !state.active &&
           !state.worker_exited);
    assert(state.phase == Stopping);
    release_cleanup = true;
    wait_phase(Failed);
    assert(!state.worker_exited);
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    wait_phase(Ready);
    store(&state.command, 0u);
    wait_phase(Stopped);
}
static void test_worker_creation_failure_is_terminal() {
    fail_thread_create = true;
    begin();
    wait_phase(Failed);
    assert(state.error == 3 && state.worker_exited == 1 && !state.manager);
    fail_thread_create = false;
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(state.phase == Failed && state.worker_exited == 1 && !state.manager);
}
static void test_shutdown_counts_attach_initialization() {
    block_create = true;
    std::thread initializer([] { begin(); });
    while (!create_waiting)
        std::this_thread::yield();
    assert(state.active >= 1 && !state.manager);
    store(&state.command, 0u);
    assert(state.active >= 1);
    release_create = true;
    initializer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(!state.active && !state.manager && !destroys);
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    wait_phase(Ready);
    store(&state.command, 0u);
    wait_phase(Stopped);
    assert(workers_created == 1);
}
static void test_shutdown_counts_worker_initialization() {
    block_admission = true;
    begin();
    while (!admission_waiting)
        std::this_thread::yield();
    assert(state.active >= 1 && !state.manager);
    store(&state.command, 0u);
    release_admission = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(!state.active && !state.manager && !destroys);
    store(&state.deadline_raw_ns, u64(now() + 2000000000ULL));
    store(&state.generation, 2u);
    store(&state.command, 1u);
    wait_phase(Ready);
    store(&state.command, 0u);
    wait_phase(Stopped);
}
static void test_concurrent_attach_creates_one_worker() {
    state.magic = Magic;
    state.version = 1;
    state.command = 1;
    state.generation = 1;
    state.deadline_raw_ns = now() + 2000000000ULL;
    const word table[]{word(frame_next),     word(imu_next),      word(connection_next),
                       word(config_next),    word(precheck_next), word(clock_next),
                       word(publisher_next), word(image_next),    word(predictions_next),
                       word(metadata_next),  word(transform_next)};
    std::atomic<bool> start{false};
    std::vector<std::thread> callers;
    for (unsigned i = 0; i < 8; ++i)
        callers.emplace_back([&] {
            while (!start)
                std::this_thread::yield();
            ct_tracker_attach(&state, &stock_manager, table);
        });
    start = true;
    for (auto &caller : callers)
        caller.join();
    wait_phase(Ready);
    assert(workers_created == 1);
    store(&state.command, 0u);
    wait_phase(Stopped);
    assert(destroys == 1);
}
int main() {
    Control lease{};
    lease.command = 1;
    lease.deadline_raw_ns = 1000000000;
    assert(leased(lease, 1) && !leased(lease, 1000000000) && !leased(lease, 0));
    lease.deadline_raw_ns = ~u64(0);
    assert(!leased(lease, 1));
    for (int scenario = 0; scenario < 22; ++scenario) {
        auto pid = fork();
        assert(pid >= 0);
        if (!pid) {
            alarm(8);
            if (scenario == 0)
                test_routing();
            else if (scenario == 1)
                test_inflight_cleanup();
            else if (scenario == 2)
                test_expired_lease();
            else if (scenario == 6)
                test_publishers_share_scratch_safely();
            else if (scenario == 7)
                test_late_config_is_owned_and_replayed();
            else if (scenario == 8)
                test_readiness_checks_share_scratch_safely();
            else if (scenario == 9)
                test_image_workers_share_native_queues_safely();
            else if (scenario == 10)
                test_remaining_pose_scratch_is_serialized();
            else if (scenario == 11)
                test_late_callback_drains_after_failure();
            else if (scenario == 12)
                test_lost_identity_retains_resources();
            else if (scenario == 13)
                test_destructor_failure_is_terminal();
            else if (scenario == 14)
                test_identity_loss_during_late_drain_is_terminal();
            else if (scenario == 15)
                test_cleanup_publishes_final_state_last();
            else if (scenario == 16)
                test_worker_creation_failure_is_terminal();
            else if (scenario == 17)
                test_shutdown_counts_attach_initialization();
            else if (scenario == 18)
                test_shutdown_counts_worker_initialization();
            else if (scenario == 19)
                test_concurrent_attach_creates_one_worker();
            else if (scenario == 20)
                test_contended_publishers_sleep();
            else if (scenario == 21) {
                failure = 5;
                test_failure();
            } else {
                failure = scenario - 2;
                test_failure();
            }
            _exit(0);
        }
        int result = 0;
        assert(waitpid(pid, &result, 0) == pid && WIFEXITED(result) && !WEXITSTATUS(result));
    }
    std::puts("native tracker routing, lifecycle, lease and failure checks passed");
}
