#include "capability.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>

namespace ct::helper {
CapabilityPublisher::CapabilityPublisher(ProtectedDirectory directory, pid_t pid, uint64_t started)
    : directory_(std::move(directory)), pid_(uint32_t(pid)), started_(started) {
    if (pid < 2 || !started)
        throw std::invalid_argument("invalid native readiness identity");
}
FileDescriptor CapabilityPublisher::existing(uint64_t now) const {
    directory_.verify();
    FileDescriptor file;
    try {
        file = directory_.file("status", sizeof(tracker::Capability));
    } catch (const std::system_error &error) {
        if (error.code().value() == ENOENT)
            return {};
        throw;
    }
    const auto bytes = read_file(file.get(), sizeof(tracker::Capability));
    if (bytes.size() != sizeof(tracker::Capability))
        throw std::runtime_error("Changed isolated native status file.");
    tracker::Capability value;
    std::memcpy(&value, bytes.data(), sizeof(value));
    if (value.magic != tracker::Magic || value.version != 1 || value.phase != 2 || value.pid < 2 ||
        !value.start || !value.generation)
        throw std::runtime_error("Changed isolated native status file.");
    if ((value.pid != pid_ || value.start != started_) && value.deadline > now)
        throw std::runtime_error("Isolated native status belongs to another process.");
    directory_.verify_file("status", file.get());
    return file;
}
bool CapabilityPublisher::publish(const tracker::Control &state, uint64_t now) {
    if (!now || now > UINT64_MAX - 500000000ULL)
        throw std::invalid_argument("invalid native readiness clock");
    if (state.magic != tracker::Magic || state.version != 1 || state.phase != tracker::Ready ||
        state.error || state.command != 1 || !state.frames || !state.generation ||
        state.deadline_raw_ns <= now || state.deadline_raw_ns - now > 2500000000ULL) {
        withdraw(now);
        return false;
    }
    const tracker::Capability value{tracker::Magic,
                                    1,
                                    2,
                                    std::min<uint64_t>(state.deadline_raw_ns, now + 500000000ULL),
                                    pid_,
                                    state.generation,
                                    started_};
    if (!tracker::valid_capability(value, now, started_))
        throw std::runtime_error("invalid native readiness capability");
    auto previous = existing(now);
    const std::string bytes(reinterpret_cast<const char *>(&value), sizeof(value));
    atomic_write(directory_.path() / "status", bytes, 0644);
    auto saved = directory_.file("status", sizeof(value));
    if (read_file(saved.get(), sizeof(value)) != bytes)
        throw std::runtime_error("native readiness publication did not read back");
    return true;
}
void CapabilityPublisher::withdraw(uint64_t now) {
    if (!now)
        throw std::invalid_argument("invalid native readiness clock");
    auto file = existing(now);
    if (!file)
        return;
    directory_.verify_file("status", file.get());
    if (unlinkat(directory_.descriptor(), "status", 0) && errno != ENOENT)
        throw std::system_error(errno, std::generic_category(), "withdraw native readiness");
}
} // namespace ct::helper
