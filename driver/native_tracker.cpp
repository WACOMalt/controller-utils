#include "../src/tracking/native_abi.h"

extern "C" {
struct CtTimespec {
    long sec, ns;
};
int ct_clock_gettime(int, CtTimespec *) asm("clock_gettime");
int ct_nanosleep(const CtTimespec *, CtTimespec *) asm("nanosleep");
int ct_pthread_create(unsigned long *, const void *, void *(*)(void *),
                      void *) asm("pthread_create");
int ct_pthread_detach(unsigned long) asm("pthread_detach");
long ct_syscall(long, ...) asm("syscall");
}

namespace ct::tracker {
static Control *control;
static void *private_manager;
static void *app;
#ifndef CT_ISOLATION_TEST
static void *stock_manager;
static word stock_classic;
#endif
static bool constructed;
static bool attached;
static word original[Entries];
static unsigned publisher_lock;
static unsigned precheck_lock;
static unsigned image_lock;
static unsigned predictions_lock, metadata_lock, transform_lock;
struct ScratchLock {
    unsigned *gate;
#if defined(__aarch64__)
    static constexpr long Futex = 98;
#else
    static constexpr long Futex = 202;
#endif
    explicit ScratchLock(unsigned &value) : gate(&value) {
        while (__atomic_exchange_n(gate, 1u, __ATOMIC_ACQUIRE))
            ct_syscall(Futex, gate, 128, 1u, nullptr);
    }
    ~ScratchLock() {
        store(gate, 0u);
        ct_syscall(Futex, gate, 129, 1);
    }
};
struct NativeString {
    const char *data;
    word length;
    word storage[2];
};
static char cached_config[65537];
static word cached_config_size;
static u32 cached_config_generation;
static unsigned config_lock;
#ifndef CT_ISOLATION_TEST
static word saved_metrics;
#endif

static u64 now() {
    CtTimespec t{};
    return ct_clock_gettime(4, &t) ? 0 : u64(t.sec) * 1000000000ULL + u64(t.ns);
}
static void pause() {
    const CtTimespec t{0, 10000000};
    ct_nanosleep(&t, nullptr);
}
static bool identity() {
#ifdef CT_ISOLATION_TEST
    extern bool test_identity();
    return test_identity();
#else
    return load(reinterpret_cast<void **>(0x3244ef8)) == app &&
           load(reinterpret_cast<word *>(app)) == 0x316b890 &&
           load(reinterpret_cast<word *>(static_cast<char *>(app) + 0x1970)) ==
               control->stock_wrapper &&
           load(reinterpret_cast<void **>(control->stock_wrapper)) == stock_manager &&
           load(reinterpret_cast<word *>(static_cast<char *>(stock_manager) + 0x2b0)) ==
               stock_classic &&
           stock_classic;
#endif
}
static void fail(u32 error) {
    store(&control->error, error);
    store(&control->command, u32(0));
}
struct Initializer {
    Control &state;
    explicit Initializer(Control &value) : state(value) {
        __atomic_add_fetch(&state.active, 1u, __ATOMIC_ACQ_REL);
    }
    ~Initializer() {
        __atomic_sub_fetch(&state.active, 1u, __ATOMIC_RELEASE);
    }
};

// Increment before checking again: the worker cannot free an admitted context.
struct Call {
    Wrapper target = nullptr;
    bool admitted = false;
    Call(Wrapper incoming, u32 id) {
        if (id != 16 || incoming == &private_manager) {
            target = incoming;
            return;
        }
        if (word(incoming) != control->stock_wrapper)
            return;
        if (load(&control->phase) != Ready || !leased(*control, now()))
            return;
        __atomic_add_fetch(&control->active, 1u, __ATOMIC_ACQ_REL);
        if (load(&control->phase) == Ready && leased(*control, now())) {
            target = &private_manager;
            admitted = true;
        } else
            __atomic_sub_fetch(&control->active, 1u, __ATOMIC_RELEASE);
    }
    ~Call() {
        if (admitted)
            __atomic_sub_fetch(&control->active, 1u, __ATOMIC_RELEASE);
    }
};

static void frame(Wrapper self, void *packet, void *head, void *acceleration, u32 a, void *aux,
                  u32 b, u32 c, void *extra1, void *extra2) {
    auto next = reinterpret_cast<FrameCall>(original[Frame]);
    next(self, packet, head, acceleration, a, aux, b, c, extra1, extra2);
    if (word(self) != control->stock_wrapper)
        return;
    Call call(self, 16);
    if (!call.target)
        return;
    const auto start = now();
    try {
        next(call.target, packet, head, acceleration, a, aux, b, c, extra1, extra2);
        __atomic_add_fetch(&control->frames, 1ULL, __ATOMIC_RELAXED);
    } catch (...) {
        fail(4);
    }
    const auto elapsed = now() - start;
    auto maximum = load(&control->max_frame_ns);
    while (elapsed > maximum &&
           !__atomic_compare_exchange_n(&control->max_frame_ns, &maximum, elapsed, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (elapsed > 10000000ULL)
        fail(5);
}
static u32 imu(Wrapper self, void *sample, void *measured, u32 id, void *output, float scalar) {
    Call call(self, id);
    if (!call.target)
        return 0;
    auto next = reinterpret_cast<ImuCall>(original[Imu]);
    if (call.target != &private_manager)
        return next(self, sample, measured, id, output, scalar);
    try {
        auto result = next(call.target, sample, measured, id, output, scalar);
        __atomic_add_fetch(&control->imu, 1ULL, __ATOMIC_RELAXED);
        return result;
    } catch (...) {
        fail(6);
        return 0;
    }
}
static void connection(Wrapper self, u32 id, u32 state) {
    Call call(self, id);
    if (!call.target)
        return;
    auto next = reinterpret_cast<ConnectionCall>(original[Connection]);
    if (call.target != &private_manager) {
        next(self, id, state);
        return;
    }
    try {
        ScratchLock lock(config_lock);
        next(call.target, id, state);
        // The capture source sends a device's config only once per XRService lifetime.
        if (state == 1 && cached_config_size) {
            NativeString json{cached_config, cached_config_size, {sizeof(cached_config) - 1, 0}};
            reinterpret_cast<ConfigurationCall>(original[Configuration])(call.target, &json, id);
            cached_config_generation = load(&control->generation);
        }
    } catch (...) {
        fail(7);
    }
}
static void configuration(Wrapper self, void *json, u32 id) {
    Call call(self, id);
    auto next = reinterpret_cast<ConfigurationCall>(original[Configuration]);
    if (call.target && call.target != &private_manager) {
        next(self, json, id);
        return;
    }
    if (!call.target && (id != 16 || word(self) != control->stock_wrapper))
        return;
    try {
        ScratchLock lock(config_lock);
        const auto &text = *static_cast<const NativeString *>(json);
        if (!text.data || !text.length || text.length >= sizeof(cached_config)) {
            if (call.target)
                fail(15);
            return;
        }
        bool same = text.length == cached_config_size;
        for (word i = 0; same && i < text.length; ++i)
            same = text.data[i] == cached_config[i];
        if (call.target && same && cached_config_generation == load(&control->generation))
            return;
        for (word i = 0; i < text.length; ++i)
            cached_config[i] = text.data[i];
        cached_config[text.length] = 0;
        cached_config_size = text.length;
        if (call.target) {
            next(call.target, json, id);
            cached_config_generation = load(&control->generation);
        }
    } catch (...) {
        fail(8);
    }
}
static u32 precheck(Wrapper self, u32 id) {
    Call call(self, id);
    if (!call.target)
        return 0;
    // Both managers copy into Valve's heap-owning static pose at 0x324f1f0.
    ScratchLock lock(precheck_lock);
    auto next = reinterpret_cast<PrecheckCall>(original[Precheck]);
    if (call.target != &private_manager)
        return next(self, id);
    try {
        return next(call.target, id);
    } catch (...) {
        fail(9);
        return 0;
    }
}
static double clock(Wrapper self, u32 id) {
    Call call(self, id);
    if (!call.target)
        return __builtin_nan("");
    auto next = reinterpret_cast<ClockCall>(original[Clock]);
    if (call.target != &private_manager)
        return next(self, id);
    try {
        return next(call.target, id);
    } catch (...) {
        fail(10);
        return __builtin_nan("");
    }
}

static void publisher(void *manager, u32 id, void *input) {
    // Valve's pose publisher uses process-global scratch storage, even though
    // filters and histories belong to each manager. Serialize every publisher.
    ScratchLock lock(publisher_lock);
    reinterpret_cast<PublisherCall>(original[Publisher])(manager, id, input);
}
static void image(void *detector, void *pixels, void *output, void *crop, void *settings,
                  void *history) {
    // Both native workers mutate the flood-fill queues at 0x3416638 and 0x3416660.
    ScratchLock lock(image_lock);
    reinterpret_cast<ImageCall>(original[Image])(detector, pixels, output, crop, settings, history);
}
static void predictions(void *manager, void *output, double timestamp) {
    // These three routines own distinct process-global poses with heap-backed covariances.
    ScratchLock lock(predictions_lock);
    reinterpret_cast<PredictionsCall>(original[Predictions])(manager, output, timestamp);
}
static void metadata(void *manager, u32 id) {
    ScratchLock lock(metadata_lock);
    reinterpret_cast<MetadataCall>(original[Metadata])(manager, id);
}
static void transform(void *manager, void *output) {
    ScratchLock lock(transform_lock);
    reinterpret_cast<TransformCall>(original[Transform])(manager, output);
}

#ifdef CT_ISOLATION_TEST
extern void test_construct(Wrapper, void *);
extern void test_start(Wrapper);
extern void test_configure(Wrapper);
extern void test_destroy(Wrapper);
static void restore_metrics() {}
static void construct() {
    test_construct(&private_manager, app);
}
static void start() {
    test_start(&private_manager);
}
static void configure() {
    test_configure(&private_manager);
}
static void destroy() {
    test_destroy(&private_manager);
}
#else
static void restore_metrics() {
    if (!identity())
        return;
    auto classic = load(&control->classic);
    if (!classic && constructed && private_manager)
        classic = *reinterpret_cast<word *>(static_cast<char *>(private_manager) + 0x2b0);
    // The factory can throw after publishing metrics but before storing its result.
    auto expected = classic ? classic + 0x9518 : load(reinterpret_cast<word *>(0x3416a80));
    if (constructed)
        __atomic_compare_exchange_n(reinterpret_cast<word *>(0x3416a80), &expected, saved_metrics,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static void construct() {
    saved_metrics = load(reinterpret_cast<word *>(0x3416a80));
    reinterpret_cast<void (*)(Wrapper, void *)>(0xeabd20)(&private_manager, app);
}
static void start() {
    reinterpret_cast<void (*)(Wrapper)>(0xead270)(&private_manager);
}
static void configure() {
    auto *manager = static_cast<char *>(private_manager);
    auto classic = *reinterpret_cast<word *>(manager + 0x2b0);
    store(&control->classic, classic);
    if (!classic || *reinterpret_cast<void **>(manager + 0x3b8) != app) {
        fail(11);
        return;
    }
    store(reinterpret_cast<unsigned char *>(manager + 0x2c8), static_cast<unsigned char>(0));
    struct String {
        const char *data;
        word length;
        char local[16];
    } preset{};
    const char name[] = "palmBootstrap";
    for (unsigned i = 0; i < sizeof(name); ++i)
        preset.local[i] = name[i];
    preset.data = preset.local;
    preset.length = sizeof(name) - 1;
    reinterpret_cast<void (*)(word, const String *)>(0x26161c0)(classic, &preset);
    auto *prerequisite = reinterpret_cast<int *>(classic + 0xd90);
    if (*prerequisite != 2) {
        fail(12);
        return;
    }
    *prerequisite = 0;
}
static void destroy() {
    reinterpret_cast<void (*)(Wrapper)>(0xeac140)(&private_manager);
}
#endif

static void *worker(void *) {
    u32 previous = 0;
    for (;;) {
        const auto generation = load(&control->generation);
        if (generation == previous || !leased(*control, now())) {
            pause();
            continue;
        }
        bool ready = false;
        {
            Initializer initialization(*control);
#ifdef CT_ISOLATION_TEST
            extern void test_worker_admission_boundary();
            test_worker_admission_boundary();
#endif
            if (load(&control->generation) != generation || !leased(*control, now()))
                continue;
            previous = generation;
            store(&control->phase, u32(Initializing));
            store(&control->error, u32(0));
            store(&control->frames, u64(0));
            store(&control->imu, u64(0));
            store(&control->max_frame_ns, u64(0));
            if (!identity()) {
                fail(13);
                store(&control->phase, u32(Failed));
                continue;
            }
            try {
                construct();
                constructed = true;
                store(&control->manager, word(private_manager));
                start();
                configure();
                restore_metrics();
                store(&control->wrapper, word(&private_manager));
                ready = !load(&control->error) && identity() && leased(*control, now());
            } catch (...) {
                // A throwing native constructor has already unwound its allocations.
                if (!constructed)
                    private_manager = nullptr;
                fail(1);
                restore_metrics();
            }
        }
        if (ready)
            store(&control->phase, u32(Ready));
        while (!load(&control->error) && leased(*control, now()) && identity())
            pause();
        store(&control->phase, u32(Stopping));
        const auto deadline = now() + 500000000ULL;
        while (load(&control->active) && now() < deadline)
            pause();
        if (!identity()) {
            store(&control->worker_exited, u32(1));
            fail(14);
            store(&control->phase, u32(Failed));
            return nullptr;
        }
        if (load(&control->active)) {
            fail(14);
            store(&control->phase, u32(Failed));
            // A late callback may still drain. Keep ownership and never admit another one.
            while (load(&control->active)) {
                if (!identity()) {
                    store(&control->worker_exited, u32(1));
                    return nullptr;
                }
                pause();
            }
            if (!identity()) {
                store(&control->worker_exited, u32(1));
                return nullptr;
            }
        }
        store(&control->phase, u32(Stopping));
        try {
            if (constructed)
                destroy();
        } catch (...) {
            store(&control->worker_exited, u32(1));
            fail(2);
            store(&control->phase, u32(Failed));
            return nullptr;
        }
        restore_metrics();
        constructed = false;
        private_manager = nullptr;
        store(&control->wrapper, word(0));
        store(&control->manager, word(0));
        store(&control->classic, word(0));
#ifdef CT_ISOLATION_TEST
        extern void test_cleanup_boundary();
        test_cleanup_boundary();
#endif
        store(&control->phase, u32(load(&control->error) ? Failed : Stopped));
    }
}
} // namespace ct::tracker

extern "C" __attribute__((visibility("default"))) void
ct_tracker_attach(ct::tracker::Control *c, void **stock,
                  const __UINTPTR_TYPE__ *trampolines) noexcept {
    using namespace ct::tracker;
    if (load(&attached) || !c || c->magic != Magic || c->version != 1 || !stock || !*stock)
        return;
    Initializer initialization(*c);
    if (!leased(*c, now()))
        return;
    bool expected = false;
    if (!__atomic_compare_exchange_n(&attached, &expected, true, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;
    control = c;
    c->stock_wrapper = word(stock);
#ifdef CT_ISOLATION_TEST
    app = stock;
#else
    app = *reinterpret_cast<void **>(static_cast<char *>(*stock) + 0x3b8);
    stock_manager = *stock;
    stock_classic = load(reinterpret_cast<word *>(static_cast<char *>(stock_manager) + 0x2b0));
#endif
    for (unsigned i = 0; i < Entries; ++i)
        original[i] = trampolines[i];
    const word entries[] = {word(frame),         word(imu),      word(connection),
                            word(configuration), word(precheck), word(clock)};
    // Once published these hooks stay resident and drop ID16 whenever inactive.
    // Removing them while delayed queue events exist could reclaim a stock slot.
    static const SharedCallbacks callbacks{publisher, image, predictions, metadata, transform};
    store(&c->callbacks, word(&callbacks));
    for (unsigned i = 0; i < FeedEntries; ++i)
        store(&c->entry[i], entries[i]);
    unsigned long thread = 0;
    if (ct_pthread_create(&thread, nullptr, worker, nullptr)) {
        store(&c->worker_exited, u32(1));
        fail(3);
        store(&c->phase, u32(Failed));
    } else
        ct_pthread_detach(thread);
}
