#include "recovery.h"
#include "worker.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace ct::helper {
namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
bool command(uint8_t value) {
    return value == '.' || value == 'r' || value == 'l' || value == '-' || value == 'v' ||
           (value >= 'A' && value <= 'P') || (value >= 0x80 && value < 0xc0);
}
void restored(const RecoveryOutput &output) {
    auto message = json::parse("{\"op\":\"native_helper_restored\",\"recovery\":true}");
    output.emit(message.get());
}
[[noreturn]] void guard(RecoveryActions &actions, int channel, int parent, int descriptor,
                        double lease) {
    if (setsid() < 0)
        _exit(126);
    for (const int signal : {SIGINT, SIGTERM, SIGHUP, SIGALRM, SIGPIPE}) {
        struct sigaction action{};
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        if (sigaction(signal, &action, nullptr))
            _exit(126);
    }
    RecoveryOutput output(descriptor);
    close(STDIN_FILENO);
    if (descriptor == STDOUT_FILENO)
        close(STDOUT_FILENO);
    bool completed = false, recover = false, failed = false;
    std::string failure;
    try {
        actions.start(output);
        double last_parent = monotonic_time();
        while (true) {
            const auto now = monotonic_time();
            actions.poll(now);
            pollfd descriptors[]{{channel, POLLIN, 0}, {parent, POLLIN, 0}};
            const auto remaining = std::max(0.0, lease - (now - last_parent));
            const int milliseconds = std::min(100, int(std::ceil(remaining * 1000)));
            const auto ready = poll(descriptors, 2, milliseconds);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                check(-1, "wait for native recovery command");
            }
            if (descriptors[1].revents & POLLIN) {
                recover = true;
                break;
            }
            if (descriptors[0].revents & (POLLIN | POLLHUP)) {
                std::array<uint8_t, 64> bytes{};
                const auto count = read(channel, bytes.data(), bytes.size());
                if (count < 0) {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    check(-1, "read native recovery command");
                }
                if (!count) {
                    recover = true;
                    break;
                }
                bool terminal = false;
                for (ssize_t i = 0; i < count; ++i) {
                    const auto value = bytes[size_t(i)];
                    if (i == count - 1 && (value == 'd' || value == 'x')) {
                        completed = value == 'd';
                        recover = value == 'x';
                        terminal = true;
                    } else if (!command(value))
                        throw std::invalid_argument("Invalid native compatibility command.");
                }
                last_parent = monotonic_time();
                for (ssize_t i = 0; i < count - (terminal ? 1 : 0); ++i)
                    actions.command(bytes[size_t(i)], last_parent);
                if (terminal) {
                    if (recover)
                        parent = -1;
                    break;
                }
            }
            if (monotonic_time() - last_parent >= lease) {
                recover = true;
                break;
            }
        }
    } catch (const std::exception &error) {
        failure = error.what();
        failed = recover = true;
    }
    if (recover && parent >= 0) {
        if (syscall(SYS_pidfd_send_signal, parent, SIGKILL, nullptr, 0) < 0 && errno != ESRCH) {
            output.error("Native recovery could not stop its own supervisor.");
            _exit(1);
        }
        pollfd exited{parent, POLLIN, 0};
        int result;
        do {
            result = poll(&exited, 1, 1000);
        } while (result < 0 && errno == EINTR);
        if (result != 1 || !(exited.revents & POLLIN)) {
            output.error("Native recovery supervisor termination was not confirmed.");
            _exit(1);
        }
    }
    try {
        actions.release_input();
    } catch (const std::exception &error) {
        failed = true;
        output.error(error.what());
    }
    if (!completed) {
        try {
            actions.restore_tracking();
            restored(output);
        } catch (const std::exception &error) {
            failed = true;
            output.error("Native recovery failed: " + std::string(error.what()));
        }
    }
    if (!failure.empty())
        output.error(failure);
    _exit(failed ? 1 : 0);
}
} // namespace
RecoveryOutput::RecoveryOutput(int descriptor) {
    struct stat info{};
    if (descriptor >= 0 && !fstat(descriptor, &info) && S_ISFIFO(info.st_mode)) {
        const int opened = open(("/proc/self/fd/" + std::to_string(descriptor)).c_str(),
                                O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (opened >= 0)
            output_ = above_stdio(opened);
    }
}
void RecoveryOutput::emit(json_object *message) const noexcept {
    if (!output_)
        return;
    try {
        auto bytes = json::encode(message, 4095) + '\n';
        write_pipe(output_.get(), bytes.data(), bytes.size());
    } catch (...) {
        constexpr char fallback[] = "{\"op\":\"native_helper_error\",\"message\":\"Recovery "
                                    "diagnostic exceeded output limit.\"}\n";
        try {
            write_pipe(output_.get(), fallback, sizeof(fallback) - 1);
        } catch (...) {
        }
    }
}
void RecoveryOutput::error(const std::string &message) const noexcept {
    try {
        auto value = json::parse("{\"op\":\"native_helper_error\"}");
        json_object_object_add(value.get(), "message",
                               json_object_new_string_len(
                                   message.data(), int(std::min(size_t(1000), message.size()))));
        emit(value.get());
    } catch (...) {
    }
}
RecoveryGuard::RecoveryGuard(RecoveryActions &actions, int output, double lease)
    : actions_(actions) {
    if (!std::isfinite(lease) || lease <= 0 || lease > 3)
        throw std::invalid_argument("invalid native recovery lease");
    require_helper_process();
    FileDescriptor parent(int(syscall(SYS_pidfd_open, getpid(), 0)));
    if (!parent)
        check(-1, "pin native recovery supervisor");
    parent = above_stdio(parent.release());
    int channels[2];
    check(pipe2(channels, O_CLOEXEC | O_NONBLOCK), "native recovery channel");
    FileDescriptor input(channels[0]), writer(channels[1]);
    input = above_stdio(input.release());
    writer = above_stdio(writer.release());
    child_ = fork();
    check(child_, "start native recovery guard");
    if (!child_) {
        writer.reset();
        guard(actions, input.get(), parent.get(), output, lease);
    }
    channel_ = std::move(writer);
}
RecoveryGuard::~RecoveryGuard() {
    try {
        finish(false, 0);
    } catch (const std::exception &error) {
        if (status_) {
            try {
                RecoveryOutput{}.error(error.what());
            } catch (...) {
            }
        }
    }
}
void RecoveryGuard::write(const std::string &bytes) {
    if (!channel_)
        throw ConnectionError("native recovery channel is closed");
    ssize_t count;
    do {
        count = write_pipe(channel_.get(), bytes.data(), bytes.size());
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        check(-1, "write native recovery command");
    if (count != ssize_t(bytes.size()))
        throw ConnectionError("native recovery command was not delivered");
}
void RecoveryGuard::heartbeat(const std::string &bytes) {
    if (bytes.empty() || bytes.size() > 64 ||
        !std::all_of(bytes.begin(), bytes.end(),
                     [](unsigned char value) { return command(value); }))
        throw std::invalid_argument("Invalid native compatibility command.");
    write(bytes);
}
void RecoveryGuard::finish(bool tracking_restored, double timeout) {
    if (!std::isfinite(timeout) || timeout < 0 || timeout > 12)
        throw std::invalid_argument("invalid native recovery deadline");
    bool delivery_failed = false;
    if (channel_) {
        try {
            write(tracking_restored ? "d" : "x");
        } catch (...) {
            delivery_failed = true;
        }
        channel_.reset();
    }
    const auto deadline = monotonic_time() + timeout;
    while (!status_) {
        int status;
        const auto result = waitpid(child_, &status, WNOHANG);
        if (result == child_) {
            status_ = status;
            break;
        }
        if (result < 0) {
            if (errno == EINTR)
                continue;
            check(-1, "reap native recovery guard");
        }
        if (monotonic_time() >= deadline)
            throw TimeoutError("native recovery has not completed");
        sleep_for(.005);
    }
    if (delivery_failed || !WIFEXITED(*status_) || WEXITSTATUS(*status_)) {
        std::exception_ptr failure;
        if (!fallback_attempted_) {
            fallback_attempted_ = true;
            try {
                actions_.release_input();
            } catch (...) {
                failure = std::current_exception();
            }
            if (!tracking_restored) {
                try {
                    actions_.restore_tracking();
                } catch (...) {
                    if (!failure)
                        failure = std::current_exception();
                }
            }
        }
        if (failure)
            std::rethrow_exception(failure);
        throw std::runtime_error(
            "Native compatibility recovery did not confirm complete restoration.");
    }
}
} // namespace ct::helper
