#include "app/instance.h"
#include "platform/linux/files.h"
#include "platform/linux/signals.h"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace {
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
int serve(const std::filesystem::path &path, int ready, int ack,
          std::optional<int> inherited = std::nullopt) {
    ct::SignalQueue signals{SIGUSR1, SIGINT, SIGTERM, SIGHUP};
    ct::ApplicationLock owner(path, inherited);
    if (!owner.primary() || !(fcntl(owner.descriptor(), F_GETFD) & FD_CLOEXEC))
        return 1;
    owner.publish();
    if (write(ready, "r", 1) != 1)
        return 2;
    for (;;) {
        pollfd event{signals.descriptor(), POLLIN, 0};
        if (poll(&event, 1, 3000) <= 0)
            return 3;
        while (auto signal = signals.receive()) {
            if (*signal == SIGUSR1) {
                if (write(ack, "s", 1) != 1)
                    return 4;
            } else
                return 0;
        }
    }
}
void conversation(const std::filesystem::path &path, const std::filesystem::path &executable,
                  bool exec) {
    int ready[2], ack[2];
    assert(pipe(ready) == 0 && pipe(ack) == 0);
    auto owner = exec ? std::make_unique<ct::ApplicationLock>(path) : nullptr;
    if (owner)
        owner->make_inheritable();
    const auto child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[0]);
        close(ack[0]);
        if (exec) {
            const auto fd = std::to_string(owner->descriptor()), r = std::to_string(ready[1]),
                       a = std::to_string(ack[1]);
            execl(executable.c_str(), executable.c_str(), "--adopt", path.c_str(), fd.c_str(),
                  r.c_str(), a.c_str(), nullptr);
            _exit(127);
        }
        _exit(serve(path, ready[1], ack[1]));
    }
    owner.reset();
    close(ready[1]);
    close(ack[1]);
    ct::FileDescriptor ready_input(ready[0]), ack_input(ack[0]);
    char value = 0;
    assert(read(ready_input.get(), &value, 1) == 1 && value == 'r');
    {
        ct::ApplicationLock secondary(path);
        assert(!secondary.primary());
        rejects([&] { secondary.publish(); });
        rejects([&] { secondary.make_inheritable(); });
        assert(!secondary.show_running(std::filesystem::canonical("/usr/bin/sleep"), 0));
        assert(secondary.show_running(executable, .2));
        assert(read(ack_input.get(), &value, 1) == 1 && value == 's');
        rejects([&] { secondary.show_running(executable, -1); });
        rejects(
            [&] { secondary.show_running(executable, std::numeric_limits<double>::quiet_NaN()); });
        assert(kill(child, SIGTERM) == 0);
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
    }
    ct::ApplicationLock replacement(path);
    assert(replacement.primary());
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 6 && std::string(argv[1]) == "--adopt")
        return serve(argv[2], std::stoi(argv[4]), std::stoi(argv[5]), std::stoi(argv[3]));
    assert(argc == 1);
    const auto root = std::filesystem::current_path() / "build" /
                      ("controller-utils-instance-" + std::to_string(getpid()));
    assert(std::filesystem::create_directory(root));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    const auto executable = std::filesystem::canonical("/proc/self/exe");
    for (unsigned i = 0; i < 100; ++i)
        conversation(root / "app.lock", executable, false);
    conversation(root / "inherited.lock", executable, true);
    {
        const auto child = fork();
        assert(child >= 0);
        if (!child) {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGUSR1);
            assert(pthread_sigmask(SIG_UNBLOCK, &mask, nullptr) == 0);
            ct::ApplicationLock early(root / "early.lock");
            rejects([&] { early.publish(); });
            const auto pid = std::to_string(getpid());
            assert(write(early.descriptor(), pid.data(), pid.size()) == ssize_t(pid.size()));
            ct::ApplicationLock secondary(root / "early.lock");
            assert(!secondary.show_running(executable, 0));
            _exit(0);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
    }
    {
        sigset_t previous;
        assert(pthread_sigmask(SIG_BLOCK, nullptr, &previous) == 0);
        {
            ct::SignalQueue signals{SIGUSR1, SIGHUP};
            assert(!signals.receive());
            assert(kill(getpid(), SIGUSR1) == 0 && signals.receive() == SIGUSR1);
            assert(kill(getpid(), SIGHUP) == 0 && signals.receive() == SIGHUP);
            ct::ApplicationLock owner(root / "owned.lock");
            owner.publish();
            owner.make_inheritable();
            const int fd = dup(owner.descriptor());
            {
                ct::ApplicationLock inherited(root / "owned.lock", fd);
                assert(inherited.primary() && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
            }
            assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
            assert(!owner.show_running(executable, 0));
            assert(unlink((root / "owned.lock").c_str()) == 0);
            ct::atomic_write(root / "owned.lock", "replacement");
            rejects([&] { owner.publish(); });
            assert(ct::read_file(root / "owned.lock", 100) == "replacement");
        }
        sigset_t restored;
        assert(pthread_sigmask(SIG_BLOCK, nullptr, &restored) == 0);
        for (int signal = 1; signal < NSIG; ++signal)
            assert(sigismember(&previous, signal) == sigismember(&restored, signal));
        rejects([] { ct::SignalQueue invalid{SIGKILL}; });
        rejects([] { ct::SignalQueue invalid{SIGSTOP}; });
        rejects([] { ct::SignalQueue invalid{0}; });
    }
    {
        ct::atomic_write(root / "original", "keep");
        std::filesystem::create_symlink(root / "original", root / "symlink");
        rejects([&] { ct::ApplicationLock invalid(root / "symlink"); });
        std::filesystem::create_hard_link(root / "original", root / "hardlink");
        rejects([&] { ct::ApplicationLock invalid(root / "hardlink"); });
        assert(ct::read_file(root / "original", 100) == "keep");
        ct::atomic_write(root / "writable", "keep", 0666);
        rejects([&] { ct::ApplicationLock invalid(root / "writable"); });
        assert(std::filesystem::create_directory(root / "unsafe"));
        assert(chmod((root / "unsafe").c_str(), 0777) == 0);
        rejects([&] { ct::ApplicationLock invalid(root / "unsafe/app.lock"); });
        assert(!std::filesystem::exists(root / "unsafe/app.lock"));
        std::filesystem::create_directory_symlink(root / "unsafe", root / "linked-directory");
        rejects([&] { ct::ApplicationLock invalid(root / "linked-directory/app.lock"); });
        assert(mkfifo((root / "fifo").c_str(), 0600) == 0);
        rejects([&] { ct::ApplicationLock invalid(root / "fifo"); });
        rejects([] { ct::ApplicationLock invalid("relative.lock"); });
        ct::ApplicationLock owner(root / "readonly.lock");
        ct::FileDescriptor readonly(open((root / "readonly.lock").c_str(), O_RDONLY | O_CLOEXEC));
        rejects([&] { ct::ApplicationLock invalid(root / "readonly.lock", readonly.get()); });
        assert(fcntl(readonly.get(), F_GETFD) >= 0);
        int pipefd[2];
        assert(pipe(pipefd) == 0);
        ct::FileDescriptor input(pipefd[0]), output(pipefd[1]);
        rejects([&] { ct::ApplicationLock invalid(root / "readonly.lock", input.get()); });
        assert(write(output.get(), "p", 1) == 1);
        char value;
        assert(read(input.get(), &value, 1) == 1 && value == 'p');
        ct::atomic_write(root / "stale.lock", "invalid-pid");
        ct::ApplicationLock stale(root / "stale.lock"), secondary(root / "stale.lock");
        assert(!secondary.show_running(executable, 0));
        ct::atomic_write(root / "other", "0");
        ct::FileDescriptor other(open((root / "other").c_str(), O_RDWR | O_CLOEXEC));
        rejects([&] { ct::ApplicationLock invalid(root / "readonly.lock", other.get()); });
        assert(fcntl(other.get(), F_GETFD) >= 0);
    }
    std::cout << "application instance: 100 pinned show lifecycles, exec lock handoff, signal "
                 "masks and file trust passed\n";
}
