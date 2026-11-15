#include "helper/capability.h"
#include "platform/linux/files.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace {
template <class Function> void rejects(Function fn, const std::string &text = "") {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(text) != std::string::npos;
    }
    assert(failed);
}
std::string encode(const ct::tracker::Capability &value) {
    return {reinterpret_cast<const char *>(&value), sizeof(value)};
}
ct::tracker::Capability saved(const std::filesystem::path &root) {
    const auto bytes = ct::read_file(root / "status", 40);
    assert(bytes.size() == 40);
    ct::tracker::Capability value;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}
} // namespace
int main() {
    using namespace ct;
    std::string pattern =
        (std::filesystem::current_path() / "build/native-capability-XXXXXX").string();
    assert(mkdtemp(pattern.data()));
    const auto root = std::filesystem::path(pattern);
    helper::CapabilityPublisher publisher(ProtectedDirectory(root, getuid()), getpid(), 1234);
    tracker::Control state{};
    state.magic = tracker::Magic;
    state.version = 1;
    state.command = 1;
    state.phase = tracker::Ready;
    state.generation = 1;
    state.frames = 1;
    uint64_t now = 10000000000ULL;
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        now += 10000000;
        state.deadline_raw_ns = now + 1500000000ULL;
        ++state.frames;
        assert(publisher.publish(state, now));
        const auto value = saved(root);
        assert(tracker::valid_capability(value, now, 1234));
        assert(value.deadline == now + 500000000 && value.pid == uint32_t(getpid()) &&
               value.generation == 1);
        struct stat info{};
        assert(!stat((root / "status").c_str(), &info) && (info.st_mode & 0777) == 0644);
    }
    state.deadline_raw_ns = now + 1234;
    assert(publisher.publish(state, now) && saved(root).deadline == now + 1234);
    publisher.withdraw(now);
    publisher.withdraw(now);
    assert(!std::filesystem::exists(root / "status"));
    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        state.deadline_raw_ns = now + 1500000000ULL;
        assert(publisher.publish(state, now));
        auto bad = state;
        if (invalid == 0)
            bad.phase = tracker::Stopped;
        if (invalid == 1)
            bad.frames = 0;
        if (invalid == 2)
            bad.command = 0;
        if (invalid == 3)
            bad.error = 1;
        if (invalid == 4)
            bad.generation = 0;
        if (invalid == 5)
            bad.deadline_raw_ns = now;
        if (invalid == 6)
            bad.deadline_raw_ns = now + 2500000001ULL;
        assert(!publisher.publish(bad, now));
        assert(!std::filesystem::exists(root / "status"));
    }
    assert(publisher.publish(state, now));
    auto foreign = saved(root);
    ++foreign.start;
    atomic_write(root / "status", encode(foreign), 0644);
    const auto old = read_file(root / "status", 40);
    rejects([&] { publisher.withdraw(now); }, "another process");
    rejects([&] { publisher.publish(state, now); }, "another process");
    assert(read_file(root / "status", 40) == old);
    publisher.withdraw(foreign.deadline);
    assert(!std::filesystem::exists(root / "status"));
    atomic_write(root / "status", encode(foreign), 0644);
    assert(publisher.publish(state, foreign.deadline) && saved(root).start == 1234);
    publisher.withdraw(now);
    foreign.magic ^= 1;
    atomic_write(root / "status", encode(foreign), 0644);
    rejects([&] { publisher.withdraw(now); }, "Changed isolated");
    std::filesystem::remove(root / "status");
    atomic_write(root / "untouched", "keep", 0600);
    std::filesystem::create_symlink(root / "untouched", root / "status");
    rejects([&] { publisher.withdraw(now); });
    rejects([&] { publisher.publish(state, now); });
    assert(read_file(root / "untouched", 8) == "keep" &&
           std::filesystem::is_symlink(root / "status"));
    std::filesystem::remove(root / "status");
    assert(publisher.publish(state, now));
    std::filesystem::create_hard_link(root / "status", root / "linked");
    rejects([&] { publisher.withdraw(now); });
    std::filesystem::remove(root / "linked");
    rejects([&] { publisher.publish(state, UINT64_MAX); }, "clock");
    publisher.withdraw(now);
    assert(raw_time_ns() > 0);
    std::filesystem::remove_all(root);
    std::cout << "native readiness: 100 renewals, wire ABI, withdrawal, expired owner recovery and "
                 "file trust checks passed\n";
}
