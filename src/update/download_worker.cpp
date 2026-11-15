#include "download.h"
#include "download_worker.h"
#include "metadata.h"
#include "platform/json.h"
#include "platform/linux/error.h"
#include "platform/linux/protected_directory.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <system_error>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern char **environ;
namespace ct::update {
namespace {
constexpr size_t MessageLimit = 8192;
void check(int error, const char *operation) {
    if (error)
        throw std::system_error(error, std::generic_category(), operation);
}
void send(int descriptor, const std::string &bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = write_pipe(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            check(count < 0 ? errno : EIO, "write download worker message");
        offset += size_t(count);
    }
}
class DownloadWorker {
    pid_t pid_ = -1;
    FileDescriptor process_, response_;
    std::optional<int> status_;
    std::string bytes_;
    bool eof_ = false;

    void stop() noexcept {
        if (pid_ <= 0 || status_)
            return;
        if (process_) {
            if (syscall(SYS_pidfd_send_signal, process_.get(), SIGKILL, nullptr, 0) &&
                errno != ESRCH)
                std::cerr << "stop download worker: " << std::strerror(errno) << '\n';
        } else if (kill(pid_, SIGKILL) && errno != ESRCH)
            std::cerr << "stop download worker: " << std::strerror(errno) << '\n';
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0) {
            if (errno != EINTR) {
                std::cerr << "reap download worker: " << std::strerror(errno) << '\n';
                return;
            }
        }
        status_ = status;
    }

  public:
    DownloadWorker(int output, json_object *request) {
        struct sigaction disposition{};
        check(sigaction(SIGCHLD, nullptr, &disposition) ? errno : 0,
              "inspect download child ownership");
        if (disposition.sa_handler != SIG_DFL || (disposition.sa_flags & SA_NOCLDWAIT))
            throw std::runtime_error("Download worker requires owned child reaping");
        const auto message = json::encode(request, MessageLimit);
        int pair[2];
        check(pipe2(pair, O_CLOEXEC) ? errno : 0, "download request pipe");
        FileDescriptor input(pair[0]), writer(pair[1]);
        input = above_stdio(input.release());
        writer = above_stdio(writer.release());
        check(pipe2(pair, O_CLOEXEC) ? errno : 0, "download response pipe");
        response_.reset(pair[0]);
        FileDescriptor response(pair[1]);
        response_ = above_stdio(response_.release());
        response = above_stdio(response.release());
        FileDescriptor target(fcntl(output, F_DUPFD_CLOEXEC, 4));
        if (!target)
            check(errno, "duplicate download destination");
        posix_spawn_file_actions_t actions;
        check(posix_spawn_file_actions_init(&actions), "download spawn actions");
        posix_spawnattr_t attributes;
        const auto initialized = posix_spawnattr_init(&attributes);
        if (initialized) {
            posix_spawn_file_actions_destroy(&actions);
            check(initialized, "download spawn attributes");
        }
        try {
            sigset_t signals;
            sigemptyset(&signals);
            check(posix_spawnattr_setsigmask(&attributes, &signals), "download signal mask");
            check(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK),
                  "download spawn flags");
            check(posix_spawn_file_actions_adddup2(&actions, input.get(), STDIN_FILENO),
                  "download stdin");
            check(posix_spawn_file_actions_adddup2(&actions, response.get(), STDOUT_FILENO),
                  "download stdout");
            check(posix_spawn_file_actions_adddup2(&actions, target.get(), 3),
                  "download destination");
            check(posix_spawn_file_actions_addclosefrom_np(&actions, 4),
                  "download inherited descriptors");
            char executable[] = "/proc/self/exe", mode[] = "--update-download-worker";
            char *arguments[] = {executable, mode, nullptr};
            check(posix_spawn(&pid_, executable, &actions, &attributes, arguments, environ),
                  "start download worker");
            process_.reset(int(syscall(SYS_pidfd_open, pid_, 0)));
            if (!process_)
                check(errno, "pin download worker");
            check(fcntl(response_.get(), F_SETFL, O_NONBLOCK) ? errno : 0,
                  "nonblocking download response");
            input.reset();
            response.reset();
            send(writer.get(), message);
        } catch (...) {
            posix_spawnattr_destroy(&attributes);
            posix_spawn_file_actions_destroy(&actions);
            stop();
            throw;
        }
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
    }
    ~DownloadWorker() {
        stop();
    }
    DownloadWorker(const DownloadWorker &) = delete;
    DownloadWorker &operator=(const DownloadWorker &) = delete;
    std::optional<json::Value> poll() {
        std::array<char, 2048> chunk;
        while (!eof_) {
            const auto count = read(response_.get(), chunk.data(), chunk.size());
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                check(errno, "read download response");
            }
            if (!count) {
                eof_ = true;
                break;
            }
            if (size_t(count) > MessageLimit - bytes_.size())
                throw std::runtime_error("Download worker response exceeded its limit");
            bytes_.append(chunk.data(), size_t(count));
        }
        if (!status_) {
            int status = 0;
            const auto result = waitpid(pid_, &status, WNOHANG);
            if (result < 0 && errno != EINTR)
                check(errno, "reap download worker");
            if (result > 0)
                status_ = status;
        }
        if (!status_ || !eof_)
            return std::nullopt;
        if (!WIFEXITED(*status_) || WEXITSTATUS(*status_))
            throw std::runtime_error("Update download worker stopped unexpectedly");
        return json::parse(bytes_, MessageLimit);
    }
    void wait() {
        pollfd descriptor{eof_ ? process_.get() : response_.get(), POLLIN | POLLHUP, 0};
        if (::poll(&descriptor, 1, 50) < 0 && errno != EINTR)
            check(errno, "wait for download worker");
    }
};
void discard(const ProtectedDirectory &parent, const std::filesystem::path &name,
             int output) noexcept {
    struct stat opened{}, current{};
    if (!fstat(output, &opened) &&
        !fstatat(parent.descriptor(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) &&
        opened.st_dev == current.st_dev && opened.st_ino == current.st_ino)
        unlinkat(parent.descriptor(), name.c_str(), 0);
}
} // namespace
HttpsDownloader::HttpsDownloader(std::filesystem::path certificates,
                                 std::function<double()> monotonic)
    : certificates_(std::move(certificates)),
      monotonic_(monotonic ? std::move(monotonic) : monotonic_time) {}
uint64_t HttpsDownloader::download(const std::string &url, const std::filesystem::path &destination,
                                   uint64_t limit, const std::atomic_bool &cancel) const {
    https_url(url);
    if (limit > MaxArchive || !destination.is_absolute() ||
        destination.lexically_normal() != destination || destination.filename().empty() ||
        destination.string().find('\0') != std::string::npos ||
        certificates_.string().find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid update download destination or limit");
    if (cancel.load())
        throw DownloadCancelled();
    ProtectedDirectory parent(destination.parent_path(), getuid());
    const auto name = destination.filename();
    FileDescriptor output(openat(parent.descriptor(), name.c_str(),
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!output)
        check(errno, "create update download");
    try {
        json::Value request(json_object_new_object());
        json_object_object_add(request.get(), "url", json_object_new_string(url.c_str()));
        json_object_object_add(request.get(), "ca", json_object_new_string(certificates_.c_str()));
        json_object_object_add(request.get(), "limit", json_object_new_uint64(limit));
        json_object_object_add(request.get(), "parent", json_object_new_int64(getpid()));
        const auto started = monotonic_();
        DownloadWorker worker(output.get(), request.get());
        for (;;) {
            if (cancel.load() || monotonic_() - started >= 120)
                throw DownloadCancelled();
            if (auto response = worker.poll()) {
                auto value = response->get();
                if (const auto error = json::find(value, "error")) {
                    const auto message = json::string(error);
                    if (json::string(json::field(value, "kind")) == "timeout")
                        throw TimeoutError(message);
                    throw std::runtime_error(message);
                }
                const auto count = json::unsigned_integer(json::field(value, "bytes"));
                struct stat image{};
                check(fstat(output.get(), &image) ? errno : 0, "inspect download size");
                if (count > limit || image.st_size < 0 || uint64_t(image.st_size) != count)
                    throw std::runtime_error("Invalid download worker result");
                parent.verify_file(name.string(), output.get());
                check(fsync(output.get()) || fsync(parent.descriptor()) ? errno : 0,
                      "flush update download");
                return count;
            }
            worker.wait();
        }
    } catch (...) {
        discard(parent, name, output.get());
        throw;
    }
}
int download_worker() {
    json::Value result(json_object_new_object());
    try {
        if (!getuid() || geteuid() != getuid())
            throw std::runtime_error("Update downloads require an unprivileged process");
        check(prctl(PR_SET_PDEATHSIG, SIGKILL) ? errno : 0, "download parent lease");
        std::string bytes;
        std::array<char, 2048> chunk;
        for (;;) {
            const auto count = read(STDIN_FILENO, chunk.data(), chunk.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                check(errno, "read download worker request");
            if (!count)
                break;
            if (size_t(count) > MessageLimit - bytes.size())
                throw std::runtime_error("Download worker request exceeded its limit");
            bytes.append(chunk.data(), size_t(count));
        }
        auto request = json::parse(bytes, MessageLimit);
        if (json::integer(json::field(request.get(), "parent")) != getppid())
            throw std::runtime_error("Download worker parent exited");
        const auto url = json::string(json::field(request.get(), "url"));
        const auto ca = json::string(json::field(request.get(), "ca"));
        const auto limit = json::unsigned_integer(json::field(request.get(), "limit"));
        https_url(url);
        struct stat image{};
        check(fstat(3, &image) ? errno : 0, "inspect download worker destination");
        const auto flags = fcntl(3, F_GETFL);
        if (limit > MaxArchive || ca.find('\0') != std::string::npos || !S_ISREG(image.st_mode) ||
            image.st_uid != getuid() || image.st_nlink != 1 || image.st_size != 0 ||
            (image.st_mode & 0777) != 0600 || flags < 0 || (flags & O_ACCMODE) != O_WRONLY ||
            (flags & O_APPEND))
            throw std::runtime_error("Invalid download worker destination");
        const auto count = download_to_descriptor(url, 3, limit, ca);
        json_object_object_add(result.get(), "bytes", json_object_new_uint64(count));
    } catch (const std::exception &error) {
        json_object_object_add(result.get(), "error", json_object_new_string(error.what()));
        json_object_object_add(result.get(), "kind",
                               json_object_new_string(dynamic_cast<const TimeoutError *>(&error)
                                                          ? "timeout"
                                                          : "error"));
    }
    try {
        send(STDOUT_FILENO, json::encode(result.get(), MessageLimit));
    } catch (const std::exception &error) {
        std::cerr << "download worker: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
} // namespace ct::update
