#include "helper/recovery.h"
#include "platform/linux/child.h"
#include "platform/linux/files.h"
#include <array>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <optional>
#include <signal.h>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>

namespace {
class Actions final : public ct::helper::RecoveryActions {
    std::filesystem::path marker_;
    pid_t owner_ = getpid();
    std::optional<double> expires_;
    std::string mode_;
    void record(const std::string &text) {
        ct::FileDescriptor file(
            open(marker_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600));
        assert(file);
        const auto bytes = text + "\n";
        assert(write(file.get(), bytes.data(), bytes.size()) == ssize_t(bytes.size()));
    }

  public:
    Actions(std::filesystem::path marker, std::string mode)
        : marker_(std::move(marker)), mode_(std::move(mode)) {}
    void start(const ct::helper::RecoveryOutput &) override {
        record("started");
    }
    void command(uint8_t value, double now) override {
        if (value == 'A') {
            record("selected");
            expires_ = now + .75;
        }
    }
    void poll(double now) override {
        if (expires_ && now >= *expires_) {
            record("expired");
            expires_.reset();
        }
    }
    void release_input() override {
        record(getpid() == owner_ ? "parent-release" : "guard-release");
        if (mode_ == "release-failure")
            throw std::runtime_error("injected input release failure");
    }
    void restore_tracking() override {
        record(getpid() == owner_ ? "parent-restore" : "guard-restore");
        if (mode_ == "restore-failure")
            throw std::runtime_error("injected tracking restoration failure");
    }
};
std::string contents(const std::filesystem::path &path) {
    return std::filesystem::exists(path) ? ct::read_file(path, 65536) : "";
}
void await(const std::filesystem::path &path, const std::string &text, double timeout = 3) {
    const auto deadline = ct::monotonic_time() + timeout;
    while (contents(path).find(text) == std::string::npos) {
        assert(ct::monotonic_time() < deadline);
        ct::sleep_for(.001);
    }
}
int helper(const std::string &mode, const std::filesystem::path &marker) {
    assert(!setpgid(0, 0));
    Actions actions(marker, mode);
    if (mode == "closed-stdio") {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
    }
    if (mode == "closed-output") {
        int descriptors[2];
        assert(!pipe2(descriptors, O_CLOEXEC));
        close(descriptors[0]);
        assert(dup2(descriptors[1], STDOUT_FILENO) == STDOUT_FILENO);
        close(descriptors[1]);
    }
    ct::helper::RecoveryGuard guard(actions, STDOUT_FILENO, mode == "stalled" ? .1 : 3);
    if (mode == "stalled")
        for (;;)
            pause();
    if (mode == "full-output") {
        ct::FileDescriptor output(open("/proc/self/fd/1", O_WRONLY | O_NONBLOCK | O_CLOEXEC));
        assert(output && !(fcntl(STDOUT_FILENO, F_GETFL) & O_NONBLOCK));
        const auto frame = "{\"padding\":\"" + std::string(4000, 'x') + "\"}\n";
        while (true) {
            const auto count = ct::write_pipe(output.get(), frame.data(), frame.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && errno == EAGAIN)
                break;
            assert(count == ssize_t(frame.size()));
        }
        guard.finish(false);
        return 0;
    }
    if (mode == "closed-output" || mode == "closed-stdio") {
        guard.finish(false);
        return 0;
    }
    std::cout << "{\"op\":\"ready\",\"guard\":" << guard.pid() << "}\n" << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "complete") {
            guard.finish(true);
            return 0;
        }
        if (line == "recover") {
            guard.finish(false);
            return 0;
        }
        if (line == "destructor")
            return 0;
        if (line == "kill-guard" || line == "kill-guard-destructor") {
            assert(!kill(guard.pid(), SIGKILL));
            if (line == "kill-guard-destructor") {
                siginfo_t exited{};
                assert(!waitid(P_PID, id_t(guard.pid()), &exited, WEXITED | WNOWAIT));
                return 1;
            }
            try {
                guard.finish(false);
            } catch (const std::exception &) {
                return 1;
            }
            assert(false);
        }
        guard.heartbeat(line);
    }
    guard.finish(false);
    return 0;
}
pid_t receive(ct::ChildProcess &child) {
    const auto deadline = ct::monotonic_time() + 3;
    while (true) {
        auto messages = child.poll();
        if (!messages.empty())
            return pid_t(ct::json::integer(ct::json::field(messages.front().get(), "guard")));
        assert(ct::monotonic_time() < deadline);
        ct::sleep_for(.001);
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "--helper") {
        try {
            return helper(argv[2], argv[3]);
        } catch (const std::exception &) {
            return 1;
        }
    }
    char directory[] = "/tmp/controller-utils-recovery-XXXXXX";
    assert(mkdtemp(directory));
    const auto root = std::filesystem::path(directory);
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    unsigned number = 0;
    auto marker = [&] { return root / std::to_string(number++); };
    auto command = [&](const char *mode, const std::filesystem::path &path) {
        return std::vector<std::string>{executable, "--helper", mode, path.string()};
    };
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        const auto path = marker();
        ct::ChildProcess child(command("normal", path), root);
        receive(child);
        child.send("A\n");
        child.close("complete\n");
        assert(contents(path) == "started\nselected\nguard-release\n");
        assert(child.exit_status() && WIFEXITED(*child.exit_status()) &&
               WEXITSTATUS(*child.exit_status()) == 0);
    }
    {
        const auto path = marker();
        ct::ChildProcess child(command("normal", path), root);
        receive(child);
        child.send("A\n");
        const auto deadline = ct::monotonic_time() + 2;
        while (contents(path).find("expired\n") == std::string::npos) {
            assert(ct::monotonic_time() < deadline);
            child.send(".\n");
            ct::sleep_for(.05);
        }
        child.close("recover\n");
        assert(contents(path) == "started\nselected\nexpired\nguard-release\nguard-restore\n");
    }
    for (const auto *mode : {"normal", "stalled", "group-signal"}) {
        const auto path = marker();
        ct::ChildProcess child(command(std::string(mode) == "group-signal" ? "normal" : mode, path),
                               root);
        if (std::string(mode) == "group-signal") {
            const auto guardian = receive(child);
            await(path, "started\n");
            assert(getsid(guardian) == guardian && getsid(child.pid()) != guardian);
        }
        await(path, "started\n");
        if (std::string(mode) == "normal")
            assert(!kill(child.pid(), SIGKILL));
        else if (std::string(mode) == "group-signal")
            assert(!kill(-child.pid(), SIGTERM));
        await(path, "guard-restore\n");
        child.close("");
        assert(WIFSIGNALED(*child.exit_status()) &&
               WTERMSIG(*child.exit_status()) ==
                   (std::string(mode) == "group-signal" ? SIGTERM : SIGKILL));
        assert(contents(path) == "started\nguard-release\nguard-restore\n");
    }
    for (const auto *mode : {"full-output", "closed-output", "closed-stdio"}) {
        const auto path = marker();
        ct::ChildProcess child(command(mode, path), root);
        await(path, "guard-restore\n");
        child.close("");
        assert(contents(path) == "started\nguard-release\nguard-restore\n");
        assert(WIFEXITED(*child.exit_status()) && !WEXITSTATUS(*child.exit_status()));
    }
    for (const auto *action : {"destructor\n", "kill-guard\n", "kill-guard-destructor\n"}) {
        const auto path = marker();
        ct::ChildProcess child(command("normal", path), root);
        receive(child);
        await(path, "started\n");
        child.close(action);
        if (std::string(action) == "destructor\n")
            assert(contents(path) == "started\nguard-release\nguard-restore\n");
        else {
            assert(contents(path) == "started\nparent-release\nparent-restore\n");
            assert(WIFEXITED(*child.exit_status()) && WEXITSTATUS(*child.exit_status()) == 1);
        }
    }
    for (const auto *mode : {"release-failure", "restore-failure"}) {
        const auto path = marker();
        ct::ChildProcess child(command(mode, path), root);
        receive(child);
        child.close("recover\n");
        assert(contents(path) ==
               "started\nguard-release\nguard-restore\nparent-release\nparent-restore\n");
        assert(WIFEXITED(*child.exit_status()) && WEXITSTATUS(*child.exit_status()) == 1);
    }
    {
        int pipes[2];
        assert(!pipe2(pipes, O_CLOEXEC));
        ct::FileDescriptor input(pipes[0]), output(pipes[1]);
        ct::helper::RecoveryOutput report(output.get());
        auto message = ct::json::parse("{\"op\":\"state\"}");
        report.emit(message.get());
        std::array<char, 1024> bytes{};
        const auto count = read(input.get(), bytes.data(), bytes.size());
        assert(count > 0 && ct::json::string(ct::json::field(
                                ct::json::parse(std::string(bytes.data(), size_t(count))).get(),
                                "op")) == "state");
        input.reset();
        report.emit(message.get());
        assert(!(fcntl(output.get(), F_GETFL) & O_NONBLOCK));
    }
    std::filesystem::remove_all(root);
    std::cout << "native guard: 100 lifecycles, supervisor death, stalls, guard death, leases and "
                 "blocked diagnostics passed\n";
}
