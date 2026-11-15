#pragma once

namespace ct::tracker {
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;
using word = __UINTPTR_TYPE__;
constexpr u64 Magic = 0x314c4f5349544343ULL;
enum Phase : u32 { Empty, Initializing, Ready, Stopping, Stopped, Failed };
enum Entry : unsigned {
    Frame,
    Imu,
    Connection,
    Configuration,
    Precheck,
    Clock,
    FeedEntries,
    Publisher = FeedEntries,
    Image,
    Predictions,
    Metadata,
    Transform,
    Entries
};
struct Control {
    u64 magic;
    u32 version, command;
    u64 deadline_raw_ns;
    word callbacks;
    word entry[FeedEntries];
    word wrapper, manager, classic, stock_wrapper;
    u32 phase, error, active, generation;
    u64 frames, imu, max_frame_ns;
    u32 loader_started, worker_exited;
};
static_assert(sizeof(Control) == 160);
static_assert(__builtin_offsetof(Control, entry) == 32);
static_assert(__builtin_offsetof(Control, phase) == 112);
static_assert(__builtin_offsetof(Control, generation) == 124);
static_assert(__builtin_offsetof(Control, loader_started) == 152);
static_assert(__builtin_offsetof(Control, worker_exited) == 156);

using Wrapper = void **;
// AAPCS64: the frame method's final two pointers arrive on the caller's stack.
using FrameCall = void (*)(Wrapper, void *, void *, void *, u32, void *, u32, u32, void *, void *);
using ImuCall = u32 (*)(Wrapper, void *, void *, u32, void *, float);
using ConnectionCall = void (*)(Wrapper, u32, u32);
using ConfigurationCall = void (*)(Wrapper, void *, u32);
using PrecheckCall = u32 (*)(Wrapper, u32);
using ClockCall = double (*)(Wrapper, u32);
using PublisherCall = void (*)(void *, u32, void *);
using ImageCall = void (*)(void *, void *, void *, void *, void *, void *);
using PredictionsCall = void (*)(void *, void *, double);
using MetadataCall = void (*)(void *, u32);
using TransformCall = void (*)(void *, void *);
struct SharedCallbacks {
    PublisherCall publisher;
    ImageCall image;
    PredictionsCall predictions;
    MetadataCall metadata;
    TransformCall transform;
};
template <class T> inline T load(const T *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}
template <class T> inline void store(T *field, T value) {
    __atomic_store_n(field, value, __ATOMIC_RELEASE);
}
inline bool leased(const Control &c, u64 now) {
    const auto deadline = load(&c.deadline_raw_ns);
    return load(&c.command) == 1 && now && deadline > now && deadline - now <= 2500000000ULL;
}
struct Capability {
    u64 magic;
    u32 version, phase;
    u64 deadline;
    u32 pid, generation;
    u64 start;
};
static_assert(sizeof(Capability) == 40);
inline bool valid_capability(const Capability &c, u64 now, u64 start) {
    return c.magic == Magic && c.version == 1 && c.phase == 2 && c.generation && c.pid > 1 &&
           c.start && c.start == start && c.deadline > now && c.deadline - now <= 500000000;
}
} // namespace ct::tracker
