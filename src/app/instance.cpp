#include "instance.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>

namespace ct {
namespace {
std::filesystem::path parent(const std::filesystem::path &path) {
    if (!path.is_absolute() || path.lexically_normal() != path || path.filename().empty() ||
        path.string().find('\0') != std::string::npos)
        throw std::invalid_argument("invalid application lock path");
    std::filesystem::create_directories(path.parent_path());
    return path.parent_path();
}
void check(int result, const char *operation) {
    if (result < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
void lock_identity(int fd, const std::filesystem::path &path) {
    struct stat opened{}, expected{};
    check(fstat(fd, &opened), "inspect application lock");
    check(lstat(path.c_str(), &expected), "inspect application lock path");
    if (!S_ISREG(opened.st_mode) || opened.st_uid != getuid() || opened.st_nlink != 1 ||
        (opened.st_mode & 0022) || !S_ISREG(expected.st_mode) || opened.st_dev != expected.st_dev ||
        opened.st_ino != expected.st_ino)
        throw std::invalid_argument("Invalid inherited Controller Utils app lock");
    const auto flags = fcntl(fd, F_GETFL);
    check(flags, "inspect application lock access");
    if ((flags & O_ACCMODE) != O_RDWR)
        throw std::invalid_argument("Controller Utils app lock must be writable");
}
bool alive(int descriptor) {
    pollfd pending{descriptor, POLLIN, 0};
    const auto result = poll(&pending, 1, 0);
    check(result, "check application process");
    return !result;
}
bool notify(int lock, const struct stat &expected) {
    const auto text = read_file(lock, 64);
    pid_t pid = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || pid <= 1)
        return false;
    FileDescriptor process(int(syscall(SYS_pidfd_open, pid, 0)));
    if (!process || !alive(process.get()))
        return false;
    FileDescriptor directory(
        open(("/proc/" + std::to_string(pid)).c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!directory)
        return false;
    struct stat owner{};
    check(fstat(directory.get(), &owner), "inspect application process owner");
    if (owner.st_uid != getuid())
        return false;
    FileDescriptor image(openat(directory.get(), "exe", O_RDONLY | O_CLOEXEC));
    FileDescriptor status(openat(directory.get(), "status", O_RDONLY | O_CLOEXEC));
    if (!image || !status)
        return false;
    struct stat opened{};
    check(fstat(image.get(), &opened), "inspect running application image");
    if (opened.st_dev != expected.st_dev || opened.st_ino != expected.st_ino)
        return false;
    std::istringstream lines(read_file(status.get(), 65536));
    std::string line;
    bool ready = false;
    while (std::getline(lines, line)) {
        if (line.rfind("SigBlk:", 0) != 0)
            continue;
        std::istringstream field(line.substr(7));
        uint64_t mask = 0;
        if (!(field >> std::hex >> mask))
            return false;
        ready = mask & (uint64_t(1) << (SIGUSR1 - 1));
    }
    if (!ready || !alive(process.get()))
        return false;
    return syscall(SYS_pidfd_send_signal, process.get(), SIGUSR1, nullptr, 0) == 0;
}
} // namespace

ApplicationLock::ApplicationLock(const std::filesystem::path &path, std::optional<int> inherited)
    : path_(path), directory_(parent(path), getuid()) {
    if (inherited) {
        if (*inherited < 3)
            throw std::invalid_argument("invalid inherited application lock descriptor");
        lock_identity(*inherited, path_);
        descriptor_.reset(*inherited);
        check(fcntl(descriptor_.get(), F_SETFD, FD_CLOEXEC), "protect inherited application lock");
    } else {
        descriptor_.reset(openat(directory_.descriptor(), path.filename().c_str(),
                                 O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
        if (!descriptor_)
            check(-1, "open application lock");
        lock_identity(descriptor_.get(), path_);
    }
    if (!flock(descriptor_.get(), LOCK_EX | LOCK_NB))
        primary_ = true;
    else if (errno != EWOULDBLOCK)
        check(-1, "acquire application lock");
}
void ApplicationLock::publish() {
    if (!primary_)
        throw std::logic_error("application lock is not owned");
    sigset_t mask;
    const auto result = pthread_sigmask(SIG_BLOCK, nullptr, &mask);
    if (result)
        throw std::system_error(result, std::generic_category(), "inspect application signal mask");
    if (sigismember(&mask, SIGUSR1) != 1)
        throw std::logic_error("application show signal is not ready");
    directory_.verify();
    lock_identity(descriptor_.get(), path_);
    check(ftruncate(descriptor_.get(), 0), "replace application lock owner");
    check(int(lseek(descriptor_.get(), 0, SEEK_SET)), "rewind application lock owner");
    const auto contents = std::to_string(getpid());
    size_t written = 0;
    while (written < contents.size()) {
        const auto count =
            write(descriptor_.get(), contents.data() + written, contents.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::system_error(count < 0 ? errno : EIO, std::generic_category(),
                                    "publish application lock owner");
        written += size_t(count);
    }
    check(fsync(descriptor_.get()), "flush application lock owner");
}
void ApplicationLock::make_inheritable() {
    if (!primary_)
        throw std::logic_error("application lock is not owned");
    check(fcntl(descriptor_.get(), F_SETFD, 0), "inherit application lock");
}
bool ApplicationLock::show_running(const std::filesystem::path &executable, double timeout) {
    if (!std::isfinite(timeout) || timeout < 0 || timeout > 15)
        throw std::invalid_argument("invalid application show timeout");
    if (primary_)
        return false;
    FileDescriptor image(open(executable.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!image)
        check(-1, "open expected application image");
    struct stat expected{};
    check(fstat(image.get(), &expected), "inspect expected application image");
    if (!S_ISREG(expected.st_mode) || (expected.st_mode & 0022))
        throw std::invalid_argument("invalid expected application image");
    const auto end = monotonic_time() + timeout;
    for (;;) {
        directory_.verify();
        lock_identity(descriptor_.get(), path_);
        try {
            if (notify(descriptor_.get(), expected))
                return true;
        } catch (const std::system_error &) {
        } catch (const std::invalid_argument &) {
        }
        const auto remaining = end - monotonic_time();
        if (remaining <= 0)
            return false;
        sleep_for(std::min(.1, remaining));
    }
}
} // namespace ct
