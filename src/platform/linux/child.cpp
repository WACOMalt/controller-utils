#include "child.h"
#include "error.h"
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <fcntl.h>
#include <sys/wait.h>

extern char **environ;
namespace ct {
namespace {
void check(int error, const char *operation) {
    if (error)
        throw std::system_error(error, std::generic_category(), operation);
}

void reap_later(pid_t pid) noexcept {
    try {
        std::thread([pid] {
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            }
        }).detach();
    } catch (const std::exception &error) {
        std::cerr << "child reaper: " << error.what() << '\n';
    }
}

} // namespace
ChildProcess::ChildProcess(const std::vector<std::string> &command,
                           const std::filesystem::path &directory) {
    if (command.empty() || command.size() > 128 || command.front().empty() ||
        command.front().front() != '/')
        throw std::invalid_argument("invalid child command");
    std::vector<char *> arguments;
    size_t size = 0;
    for (const auto &argument : command) {
        size += argument.size() + 1;
        if (argument.find('\0') != std::string::npos || size > 131072)
            throw std::invalid_argument("invalid child argument");
        arguments.push_back(const_cast<char *>(argument.c_str()));
    }
    arguments.push_back(nullptr);
    const auto cwd = directory.string();
    if (cwd.empty() || cwd.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid child working directory");
    int pair[2];
    if (pipe2(pair, O_CLOEXEC))
        check(errno, "child input pipe");
    FileDescriptor child_input(pair[0]), parent_input(pair[1]);
    if (pipe2(pair, O_CLOEXEC))
        check(errno, "child output pipe");
    FileDescriptor parent_output(pair[0]), child_output(pair[1]);
    child_input = above_stdio(child_input.release());
    parent_input = above_stdio(parent_input.release());
    parent_output = above_stdio(parent_output.release());
    child_output = above_stdio(child_output.release());
    posix_spawn_file_actions_t actions;
    check(posix_spawn_file_actions_init(&actions), "child spawn actions");
    try {
        check(posix_spawn_file_actions_adddup2(&actions, child_input.get(), STDIN_FILENO),
              "child stdin");
        check(posix_spawn_file_actions_adddup2(&actions, child_output.get(), STDOUT_FILENO),
              "child stdout");
        check(posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str()), "child directory");
        check(posix_spawn_file_actions_addclosefrom_np(&actions, 3), "child inherited descriptors");
        check(posix_spawn(&pid_, arguments.front(), &actions, nullptr, arguments.data(), environ),
              "start child");
    } catch (...) {
        posix_spawn_file_actions_destroy(&actions);
        throw;
    }
    posix_spawn_file_actions_destroy(&actions);
    input_ = std::move(parent_input);
    output_ = std::move(parent_output);
    for (const int descriptor : {input_.get(), output_.get()}) {
        const int flags = fcntl(descriptor, F_GETFL);
        if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK)) {
            const int error = errno;
            close_input();
            output_.reset();
            reap_later(std::exchange(pid_, -1));
            check(error, "nonblocking child channel");
        }
    }
}
ChildProcess::~ChildProcess() {
    close_input();
    output_.reset();
    if (pid_ <= 0 || status_)
        return;
    try {
        if (exit_status())
            return;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    // A helper may still be restoring native resources. Reap it without sending a signal.
    reap_later(std::exchange(pid_, -1));
}
std::optional<int> ChildProcess::exit_status() {
    if (status_)
        return status_;
    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid_, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        check(errno, "reap child");
    if (result)
        status_ = status;
    return status_;
}
void ChildProcess::send(const std::string &bytes) {
    if (input_closed_)
        throw ConnectionError("child input is closed");
    if (bytes.size() > MessageLimit)
        throw std::invalid_argument("child input exceeded limit");
    const double deadline = monotonic_time() + .1;
    size_t sent = 0;
    while (sent < bytes.size()) {
        if (monotonic_time() >= deadline)
            throw TimeoutError("child input stalled");
        const auto count = write_pipe(input_.get(), bytes.data() + sent, bytes.size() - sent);
        if (count > 0) {
            sent += size_t(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{input_.get(), POLLOUT, 0};
            if (::poll(&descriptor, 1, 10) < 0 && errno != EINTR)
                check(errno, "wait for child input");
            continue;
        }
        throw ConnectionError("child input closed");
    }
}
void ChildChannel::send(json_object *message) {
    if (!message)
        throw std::invalid_argument("invalid child message");
    send(json::encode(message, ChildProcess::MessageLimit - 1) + '\n');
}
std::vector<json::Value> ChildChannel::close(json_object *command, double timeout) {
    if (!command)
        throw std::invalid_argument("invalid child message");
    return close(json::encode(command, ChildProcess::MessageLimit - 1) + '\n', timeout);
}
void ChildProcess::read() {
    if (failure_)
        throw std::runtime_error(*failure_);
    if (output_closed_)
        return;
    try {
        std::array<char, 16384> bytes;
        size_t budget = 256 * 1024;
        while (budget) {
            const auto count = ::read(output_.get(), bytes.data(), std::min(bytes.size(), budget));
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                check(errno, "read child output");
            }
            if (!count) {
                output_closed_ = true;
                if (!buffer_.empty())
                    throw std::runtime_error("child message exceeded limit");
                break;
            }
            budget -= size_t(count);
            buffer_.append(bytes.data(), size_t(count));
            size_t begin = 0;
            while (true) {
                const auto end = buffer_.find('\n', begin);
                if (end == std::string::npos)
                    break;
                const size_t length = end - begin + 1;
                if (length > MessageLimit)
                    throw std::runtime_error("child message exceeded limit");
                if (messages_.size() >= 128 || queued_bytes_ + length > 2 * MessageLimit)
                    throw std::runtime_error("child message queue exceeded limit");
                auto value = json::parse(buffer_.substr(begin, length), MessageLimit);
                if (!json_object_is_type(value.get(), json_type_object))
                    throw std::runtime_error("invalid child message");
                messages_.emplace_back(std::move(value), length);
                queued_bytes_ += length;
                begin = end + 1;
            }
            buffer_.erase(0, begin);
            if (buffer_.size() >= MessageLimit)
                throw std::runtime_error("child message exceeded limit");
        }
    } catch (const std::exception &error) {
        failure_ = error.what();
        throw;
    }
}
std::vector<json::Value> ChildProcess::poll() {
    read();
    std::vector<json::Value> result;
    while (!messages_.empty()) {
        queued_bytes_ -= messages_.front().second;
        result.push_back(std::move(messages_.front().first));
        messages_.pop_front();
    }
    return result;
}
void ChildProcess::close_input() noexcept {
    input_.reset();
    input_closed_ = true;
}
std::vector<json::Value> ChildProcess::close(const std::string &command, double timeout) {
    if (!std::isfinite(timeout) || timeout < 0 || timeout > 12)
        throw std::invalid_argument("invalid child cleanup deadline");
    std::exception_ptr failure;
    if (failure_)
        failure = std::make_exception_ptr(std::runtime_error(*failure_));
    if (!input_closed_) {
        try {
            if (!exit_status())
                send(command);
        } catch (...) {
            failure = std::current_exception();
        }
        close_input();
    }
    std::vector<json::Value> result;
    size_t result_bytes = 0;
    const double deadline = monotonic_time() + timeout;
    do {
        if (failure_) {
            std::array<char, 16384> discard;
            for (unsigned i = 0; i < 16; ++i) {
                const auto count = ::read(output_.get(), discard.data(), discard.size());
                if (!count)
                    output_closed_ = true;
                if (count <= 0)
                    break;
            }
        } else {
            try {
                for (auto &message : poll()) {
                    result_bytes += json::encode(message.get(), MessageLimit).size();
                    if (result.size() >= 1024 || result_bytes > 2 * MessageLimit)
                        throw std::runtime_error("child cleanup output exceeded limit");
                    result.push_back(std::move(message));
                }
            } catch (...) {
                if (!failure)
                    failure = std::current_exception();
            }
        }
        if (exit_status() && (output_closed_ || failure_))
            break;
        if (monotonic_time() >= deadline)
            throw TimeoutError("child cleanup has not completed");
        if (output_closed_)
            sleep_for(.02);
        else {
            pollfd descriptor{output_.get(), POLLIN, 0};
            if (::poll(&descriptor, 1, 20) < 0 && errno != EINTR)
                check(errno, "wait for child cleanup");
        }
    } while (true);
    if (failure)
        std::rethrow_exception(failure);
    return result;
}
} // namespace ct
