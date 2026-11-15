#include "signals.h"
#include <cerrno>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <sys/signalfd.h>

namespace ct {
SignalQueue::SignalQueue(std::initializer_list<int> signals) {
    sigset_t mask;
    sigemptyset(&mask);
    for (const auto signal : signals)
        if (signal == SIGKILL || signal == SIGSTOP || sigaddset(&mask, signal))
            throw std::invalid_argument("invalid queued signal");
    const auto result = pthread_sigmask(SIG_BLOCK, &mask, &previous_);
    if (result)
        throw std::system_error(result, std::generic_category(), "block queued signals");
    events_.reset(signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
    if (!events_) {
        const auto error = errno;
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
        throw std::system_error(error, std::generic_category(), "create signal queue");
    }
}
SignalQueue::~SignalQueue() {
    events_.reset();
    pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
}
std::optional<int> SignalQueue::receive() {
    signalfd_siginfo event{};
    const auto count = read(events_.get(), &event, sizeof(event));
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return std::nullopt;
    if (count < 0)
        throw std::system_error(errno, std::generic_category(), "read signal queue");
    if (count != sizeof(event))
        throw std::runtime_error("invalid queued signal event");
    return int(event.ssi_signo);
}
} // namespace ct
