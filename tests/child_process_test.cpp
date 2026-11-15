#include "platform/linux/child.h"
#include "platform/linux/error.h"
#include <array>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <limits>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>

namespace {
void write_all(const std::string &bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            _exit(0);
        offset += size_t(count);
    }
}
int fake_helper(const std::string &mode) {
    if (mode == "partial") {
        write_all("{\"partial\":true}");
        return 0;
    }
    if (mode == "invalid") {
        write_all("invalid JSON\n");
        return 0;
    }
    if (mode == "array") {
        write_all("[]\n");
        return 0;
    }
    if (mode == "oversized") {
        write_all(std::string(ct::ChildProcess::MessageLimit, 'x'));
        return 0;
    }
    if (mode == "flood") {
        for (unsigned i = 0; i < 20000; ++i)
            write_all("{\"flood\":true}\n");
        return 0;
    }
    if (mode == "stall") {
        ct::sleep_for(.3);
        return 0;
    }
    if (mode == "closed-output") {
        close(STDOUT_FILENO);
        ct::sleep_for(.15);
        return 0;
    }
    if (mode == "closed-input") {
        close(STDIN_FILENO);
        write_all("{\"op\":\"input_closed\"}\n");
        ct::sleep_for(.15);
        return 0;
    }
    if (mode == "guard-output" || mode == "concurrent-output") {
        const auto guard = fork();
        assert(guard >= 0);
        if (!guard) {
            ct::FileDescriptor output(open("/proc/self/fd/1", O_WRONLY | O_NONBLOCK | O_CLOEXEC));
            assert(output);
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            ct::sleep_for(mode == "guard-output" ? .15 : 0);
            const std::string frame =
                "{\"op\":\"guard\",\"padding\":\"" + std::string(1000, 'g') + "\"}\n";
            const unsigned count = mode == "guard-output" ? 1 : 40;
            for (unsigned i = 0; i < count; ++i) {
                while (true) {
                    const auto written = write(output.get(), frame.data(), frame.size());
                    if (written == ssize_t(frame.size()))
                        break;
                    assert(written < 0 && (errno == EAGAIN || errno == EINTR));
                    ct::sleep_for(.001);
                }
            }
            _exit(0);
        }
        for (unsigned i = 0; i < (mode == "guard-output" ? 1u : 40u); ++i)
            write_all("{\"op\":\"supervisor\",\"padding\":\"" + std::string(1000, 's') + "\"}\n");
        if (mode == "concurrent-output") {
            int status;
            assert(waitpid(guard, &status, 0) == guard && WIFEXITED(status) &&
                   !WEXITSTATUS(status));
        }
        return 0;
    }
    if (mode == "descriptors") {
        assert(fcntl(100, F_GETFD) == -1 && errno == EBADF);
        write_all("{\"op\":\"no_inherited_descriptor\"}\n");
        return 0;
    }
    write_all("{\"op\":\"ready\"}\n");
    std::array<char, 1024> bytes;
    while (true) {
        const auto count = read(STDIN_FILENO, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        if (std::string(bytes.data(), size_t(count)).find('q') != std::string::npos) {
            write_all("{\"op\":\"restored\"}\n");
            return 0;
        }
        write_all("{\"op\":\"heartbeat\"}\n");
    }
    write_all("{\"op\":\"eof_restored\"}\n");
    return 0;
}
template <class Function> void rejects(Function function, const std::string &text) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &error) {
        rejected = std::string(error.what()).find(text) != std::string::npos;
    }
    assert(rejected);
}
size_t descriptor_count() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator());
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--helper")
        return fake_helper(argv[2]);
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    const auto directory = std::filesystem::current_path();
    auto command = [&](const char *mode) {
        return std::vector<std::string>{executable, "--helper", mode};
    };
    rejects([&] { ct::ChildProcess child({"relative"}, directory); }, "invalid child command");
    rejects([&] { ct::ChildProcess child({"/missing/controller-utils-child"}, directory); },
            "start child");
    rejects([&] { ct::ChildProcess child(command("normal"), "/missing/child-directory"); },
            "start child");
    const auto descriptors = descriptor_count();
    for (unsigned i = 0; i < 100; ++i) {
        ct::ChildProcess child(command("normal"), directory);
        assert(!child.exit_status());
        child.send(".");
        const auto messages = child.close("q");
        assert(messages.size() >= 2 && messages.size() <= 3);
        assert(ct::json::string(ct::json::field(messages.front().get(), "op")) == "ready");
        assert(ct::json::string(ct::json::field(messages.back().get(), "op")) == "restored");
        assert(child.exit_status() && WIFEXITED(*child.exit_status()) &&
               WEXITSTATUS(*child.exit_status()) == 0);
        assert(child.close("q").empty());
        rejects([&] { child.send("."); }, "child input is closed");
    }
    assert(descriptor_count() == descriptors);
    {
        ct::FileDescriptor original(open("/dev/null", O_RDONLY));
        assert(original);
        ct::FileDescriptor inherited(fcntl(original.get(), F_DUPFD, 100));
        assert(inherited.get() == 100);
        ct::ChildProcess child(command("descriptors"), directory);
        const auto messages = child.close("");
        assert(messages.size() == 1);
        assert(ct::json::string(ct::json::field(messages[0].get(), "op")) ==
               "no_inherited_descriptor");
    }
    for (const auto *mode : {"partial", "invalid", "array", "oversized", "flood"}) {
        ct::ChildProcess child(command(mode), directory);
        const auto start = ct::monotonic_time();
        rejects([&] { child.close("", 2); }, mode == std::string("array") ? "invalid child message"
                                             : mode == std::string("invalid") ? "invalid JSON"
                                                                              : "limit");
        assert(ct::monotonic_time() - start < 2);
        assert(child.exit_status());
    }
    {
        ct::ChildProcess child(command("stall"), directory);
        rejects([&] { child.send(std::string(1024 * 1024, 'x')); }, "child input stalled");
        child.close("");
        assert(WIFEXITED(*child.exit_status()));
    }
    {
        ct::ChildProcess child(command("stall"), directory);
        rejects([&] { child.close("q", .01); }, "cleanup has not completed");
        assert(!child.exit_status());
        child.close("");
    }
    {
        ct::ChildProcess child(command("closed-output"), directory);
        const auto before = ct::monotonic_time();
        child.close("");
        assert(ct::monotonic_time() - before >= .1);
    }
    for (const char *mode : {"guard-output", "concurrent-output"}) {
        ct::ChildProcess child(command(mode), directory);
        const auto before = ct::monotonic_time();
        const auto messages = child.close("");
        unsigned guards = 0, supervisors = 0;
        for (const auto &message : messages) {
            const auto operation = ct::json::string(ct::json::field(message.get(), "op"));
            guards += operation == "guard";
            supervisors += operation == "supervisor";
        }
        assert(guards == supervisors && guards == (std::string(mode) == "guard-output" ? 1 : 40));
        if (std::string(mode) == "guard-output")
            assert(ct::monotonic_time() - before >= .1);
    }
    for (bool existing_signal : {false, true}) {
        sigset_t block, previous, pending, after;
        sigemptyset(&block);
        sigaddset(&block, SIGPIPE);
        assert(!pthread_sigmask(SIG_BLOCK, &block, &previous));
        if (existing_signal)
            assert(!raise(SIGPIPE));
        {
            ct::ChildProcess child(command("closed-input"), directory);
            const double deadline = ct::monotonic_time() + 2;
            while (child.poll().empty()) {
                assert(ct::monotonic_time() < deadline);
                ct::sleep_for(.001);
            }
            rejects([&] { child.send("."); }, "child input closed");
            assert(!sigpending(&pending));
            assert(bool(sigismember(&pending, SIGPIPE)) == existing_signal);
            assert(!pthread_sigmask(SIG_SETMASK, nullptr, &after));
            assert(sigismember(&after, SIGPIPE));
            child.close("");
        }
        if (existing_signal) {
            const timespec immediate{};
            assert(sigtimedwait(&block, nullptr, &immediate) == SIGPIPE);
        }
        assert(!pthread_sigmask(SIG_SETMASK, &previous, nullptr));
    }
    {
        ct::ChildProcess child(command("normal"), directory);
        assert(!kill(child.pid(), SIGKILL));
        const auto messages = child.close("");
        assert(messages.size() <= 1 && WIFSIGNALED(*child.exit_status()));
    }
    {
        ct::ChildProcess child(command("normal"), directory);
        auto invalid = ct::json::parse("{\"value\":0}");
        json_object_object_add(invalid.get(), "value",
                               json_object_new_double(std::numeric_limits<double>::infinity()));
        rejects([&] { child.send(invalid.get()); }, "finite");
        child.close("q");
    }
    assert(descriptor_count() == descriptors);
    std::cout
        << "child ownership: 100 lifecycles, EOF recovery, bounded IO and reap checks passed\n";
}
