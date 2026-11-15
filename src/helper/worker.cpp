#include "worker.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace ct::helper {
namespace {
struct Reply {
    uint32_t magic = 0x31575043, status = 1;
    int32_t result = 0;
    uint32_t detached = 0;
    std::array<char, 1008> error{};
};
static_assert(sizeof(Reply) == 1024);
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
} // namespace
void require_helper_process() {
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task")) {
        (void)entry;
        if (++count > 1)
            throw std::logic_error("native patch workers require a single-threaded supervisor");
    }
    struct sigaction action{};
    check(sigaction(SIGCHLD, nullptr, &action), "read native worker signal disposition");
    if (action.sa_handler != SIG_DFL || (action.sa_flags & SA_NOCLDWAIT))
        throw std::logic_error("native patch worker requires exclusive child ownership");
}
namespace {
[[noreturn]] void execute(PinnedProcess &process, int output, pid_t parent,
                          const std::function<PatchResult(StoppedProcess &)> &operation,
                          double timeout) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
        _exit(126);
    Reply reply;
    std::unique_ptr<StoppedProcess> stopped;
    try {
        stopped = std::make_unique<StoppedProcess>(process, timeout * .75);
        reply.result = int32_t(operation(*stopped));
        reply.status = 0;
    } catch (const std::exception &error) {
        std::strncpy(reply.error.data(), error.what(), reply.error.size() - 1);
    } catch (...) {
        std::strcpy(reply.error.data(), "unknown native patch failure");
    }
    try {
        if (stopped)
            stopped->close(std::min(.3, timeout * .2));
        reply.detached = 1;
    } catch (...) {
        // Kernel cleanup on worker death releases trace ownership without signalling the target.
        _exit(125);
    }
    ssize_t count;
    do {
        count = write(output, &reply, sizeof(reply));
    } while (count < 0 && errno == EINTR);
    _exit(count == sizeof(reply) ? int(reply.status) : 124);
}
} // namespace
PatchResult run_patch_worker(PinnedProcess &process,
                             const std::function<PatchResult(StoppedProcess &)> &operation,
                             double timeout) {
    if (!operation || !std::isfinite(timeout) || timeout <= 0 || timeout > 1.5)
        throw std::invalid_argument("invalid native patch worker deadline");
    require_helper_process();
    process.refresh();
    int channels[2];
    check(pipe2(channels, O_CLOEXEC | O_NONBLOCK), "native patch worker channel");
    FileDescriptor input(channels[0]), output(channels[1]);
    input = above_stdio(input.release());
    output = above_stdio(output.release());
    const auto parent = getpid();
    const double deadline = monotonic_time() + timeout;
    const auto child = fork();
    check(child, "start native patch worker");
    if (!child) {
        input.reset();
        execute(process, output.get(), parent, operation, timeout);
    }
    output.reset();
    FileDescriptor identity(int(syscall(SYS_pidfd_open, child, 0)));
    bool reaped = false;
    auto stop = [&] {
        if (reaped)
            return;
        // An unreaped direct child cannot have its PID reused.
        const auto signalled =
            identity ? syscall(SYS_pidfd_send_signal, identity.get(), SIGKILL, nullptr, 0)
                     : kill(child, SIGKILL);
        if (signalled < 0 && errno != ESRCH)
            check(-1, "stop owned native patch worker");
        const double end = monotonic_time() + .3;
        int status = 0;
        do {
            const auto waited = waitpid(child, &status, WNOHANG);
            if (waited == child) {
                reaped = true;
                return;
            }
            if (waited < 0 && errno != EINTR)
                check(-1, "reap owned native patch worker");
            sleep_for(.001);
        } while (monotonic_time() < end);
        throw TimeoutError("native patch worker termination was not confirmed");
    };
    try {
        if (!identity)
            check(-1, "pin native patch worker");
        std::array<uint8_t, sizeof(Reply) + 1> bytes{};
        size_t received = 0;
        bool eof = false;
        int status = 0;
        while (!reaped || !eof) {
            if (monotonic_time() >= deadline)
                throw TimeoutError("native patch worker exceeded its bounded pause");
            const auto count = read(input.get(), bytes.data() + received, bytes.size() - received);
            if (count > 0) {
                received += size_t(count);
                if (received > sizeof(Reply))
                    throw std::runtime_error("native patch worker returned excess output");
            } else if (!count)
                eof = true;
            else if (errno != EAGAIN && errno != EINTR)
                check(-1, "read native patch worker");
            if (!reaped) {
                const auto waited = waitpid(child, &status, WNOHANG);
                if (waited == child)
                    reaped = true;
                else if (waited < 0 && errno != EINTR)
                    check(-1, "reap native patch worker");
            }
            if (reaped && eof)
                break;
            pollfd descriptors[]{{input.get(), POLLIN, 0}, {identity.get(), POLLIN, 0}};
            const int milliseconds =
                std::min(20, int(std::ceil(std::max(0.0, deadline - monotonic_time()) * 1000)));
            if (poll(descriptors, 2, milliseconds) < 0 && errno != EINTR)
                check(-1, "wait for native patch worker");
        }
        Reply reply;
        if (received != sizeof(reply))
            throw std::runtime_error("native patch worker did not confirm detachment");
        std::memcpy(&reply, bytes.data(), sizeof(reply));
        if (reply.magic != 0x31575043 || reply.detached != 1 || reply.status > 1 ||
            reply.error.back() || !WIFEXITED(status) || WEXITSTATUS(status) != int(reply.status))
            throw std::runtime_error("invalid native patch worker result");
        if (reply.status)
            throw std::runtime_error(reply.error.data());
        if (reply.result < int32_t(PatchResult::Applied) ||
            reply.result > int32_t(PatchResult::Restored))
            throw std::runtime_error("invalid native patch worker outcome");
        process.refresh();
        return PatchResult(reply.result);
    } catch (...) {
        const auto failure = std::current_exception();
        stop();
        std::rethrow_exception(failure);
    }
}
void patch_gate(PinnedProcess &process, const GateLayout &layout, bool apply) {
    run_patch_worker(process, [&](StoppedProcess &stopped) {
        validate_gate_mappings(layout, process.image_base(), process.mappings());
        TraceMemory memory(process, stopped);
        GatePatch patch(memory, layout, process.image_base());
        if (apply) {
            patch.apply();
            return PatchResult::Applied;
        }
        patch.restore([&] {
            for (const auto thread : stopped.threads()) {
                const auto destination = gate_resume_pc(patch.layout(), process.image_base(),
                                                        stopped.registers(thread).pc());
                if (destination)
                    stopped.move_pc(thread, *destination);
            }
        });
        return PatchResult::Restored;
    });
}
} // namespace ct::helper
