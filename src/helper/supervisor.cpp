#include "supervisor.h"
#include "platform/linux/error.h"
#include "platform/linux/signals.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <exception>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>

namespace ct::helper {
namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
bool valid(uint8_t value) {
    return value == '.' || value == 'r' || value == 'l' || value == '-' ||
           (value >= 'A' && value <= 'P') || (value == 'v' || (value >= 0x80 && value < 0xc0));
}
} // namespace
Supervisor::Supervisor(TrackingBackend &backend, RecoveryGuard &guard, int input, int output)
    : backend_(backend), guard_(guard), output_(output), input_(input) {}
void Supervisor::run() {
    SignalQueue signals{SIGINT, SIGTERM, SIGHUP, SIGALRM};
    FileDescriptor input;
    std::exception_ptr primary, cleanup;
    try {
        struct stat info{};
        check(fstat(input_, &info), "inspect native helper input");
        if (!S_ISFIFO(info.st_mode))
            throw std::invalid_argument("native helper input must be a pipe");
        input.reset(open(("/proc/self/fd/" + std::to_string(input_)).c_str(),
                         O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (!input)
            check(-1, "open native helper input");
        double last_input = monotonic_time(), next_attempt = 0;
        bool started = false, requested = true;
        for (;;) {
            const auto now = monotonic_time();
            const double timeout = std::min({.1, std::max(0.0, last_input + 2 - now),
                                             started ? .1 : std::max(0.0, next_attempt - now)});
            pollfd events[]{{input.get(), POLLIN, 0},
                            {backend_.descriptor(), POLLIN, 0},
                            {signals.descriptor(), POLLIN, 0}};
            const auto ready = poll(events, 3, int(std::ceil(timeout * 1000)));
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                check(-1, "wait for native helper input");
            }
            if (events[1].revents)
                throw ConnectionError("Native tracking service restarted. Reconnect the session.");
            if (events[2].revents && signals.receive())
                throw std::runtime_error("Native compatibility session interrupted.");
            if (events[0].revents & (POLLIN | POLLHUP)) {
                std::array<uint8_t, 64> bytes{};
                const auto count = read(input.get(), bytes.data(), bytes.size());
                if (count < 0) {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    check(-1, "read native helper input");
                }
                if (!count || std::find(bytes.begin(), bytes.begin() + count, uint8_t('q')) !=
                                  bytes.begin() + count)
                    break;
                for (ssize_t i = 0; i < count; ++i)
                    if (!valid(bytes[size_t(i)]))
                        throw std::invalid_argument("Invalid native compatibility command.");
                last_input = monotonic_time();
                guard_.heartbeat(
                    std::string(reinterpret_cast<const char *>(bytes.data()), size_t(count)));
                if (started)
                    backend_.renew();
                for (ssize_t i = 0; i < count; ++i) {
                    if (bytes[size_t(i)] == 'r')
                        requested = true;
                    else if (bytes[size_t(i)] == 'l')
                        requested = false;
                }
            }
            if (monotonic_time() - last_input >= 2)
                throw TimeoutError("Native compatibility heartbeat expired.");
            if (!started && monotonic_time() >= next_attempt) {
                started = backend_.start();
                next_attempt = monotonic_time() + .02;
            }
            if (started)
                for (auto &message : backend_.poll(requested))
                    output_.emit(message.get());
        }
    } catch (...) {
        primary = std::current_exception();
    }
    try {
        backend_.restore([&] {
            try {
                guard_.heartbeat();
            } catch (const std::exception &) {
            }
        });
    } catch (...) {
        cleanup = std::current_exception();
    }
    try {
        guard_.finish(!cleanup);
    } catch (...) {
        if (!cleanup)
            cleanup = std::current_exception();
    }
    if (cleanup)
        std::rethrow_exception(cleanup);
    auto restored = json::parse("{\"op\":\"native_helper_restored\"}");
    json_object_object_add(restored.get(), "isolated", json_object_new_boolean(true));
    json_object_object_add(restored.get(), "resident_hooks", json_object_new_boolean(true));
    output_.emit(restored.get());
    if (primary)
        std::rethrow_exception(primary);
}
} // namespace ct::helper
