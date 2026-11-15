#include "resource.h"
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <system_error>
#include <time.h>

namespace ct {
namespace {
void pipe_error(int value, const char *operation) {
    if (value)
        throw std::system_error(value, std::generic_category(), operation);
}
double read_clock(clockid_t clock) {
    timespec value{};
    if (clock_gettime(clock, &value))
        throw std::system_error(errno, std::generic_category(), "clock_gettime");
    return value.tv_sec + value.tv_nsec * 1e-9;
}
} // namespace
FileDescriptor above_stdio(int descriptor) {
    FileDescriptor result(descriptor);
    if (descriptor < 3) {
        const int copy = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
        if (copy < 0)
            throw std::system_error(errno, std::generic_category(), "duplicate process channel");
        result.reset(copy);
    }
    return result;
}
double monotonic_time() {
    return read_clock(CLOCK_MONOTONIC);
}
double raw_time() {
    return read_clock(CLOCK_MONOTONIC_RAW);
}
uint64_t raw_time_ns() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value))
        throw std::system_error(errno, std::generic_category(), "read raw lease clock");
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000 ||
        uint64_t(value.tv_sec) > (UINT64_MAX - uint64_t(value.tv_nsec)) / 1000000000ULL)
        throw std::runtime_error("invalid raw lease clock");
    return uint64_t(value.tv_sec) * 1000000000ULL + uint64_t(value.tv_nsec);
}
void sleep_for(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 60)
        throw std::invalid_argument("invalid bounded wait");
    timespec remaining{time_t(seconds), long((seconds - std::floor(seconds)) * 1e9)};
    while (nanosleep(&remaining, &remaining))
        if (errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "nanosleep");
}
ssize_t write_pipe(int descriptor, const void *data, size_t size) {
    sigset_t block, previous, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    pipe_error(pthread_sigmask(SIG_BLOCK, &block, &previous), "block pipe signal");
    if (sigpending(&pending)) {
        const auto error = errno;
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        pipe_error(error, "read pending pipe signal");
    }
    const auto count = write(descriptor, data, size);
    const auto error = errno;
    if (count < 0 && error == EPIPE && !sigismember(&pending, SIGPIPE)) {
        const timespec immediate{};
        while (sigtimedwait(&block, nullptr, &immediate) < 0 && errno == EINTR) {
        }
    }
    pipe_error(pthread_sigmask(SIG_SETMASK, &previous, nullptr), "restore pipe signal mask");
    errno = error;
    return count;
}
} // namespace ct
