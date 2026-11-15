#include "helper/supervisor.h"
#include "platform/linux/child.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <signal.h>
#include <string>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace {
class Host final : public ct::helper::TrackingBackend, public ct::helper::RecoveryActions {
    std::filesystem::path marker_;
    std::string mode_;
    pid_t owner_ = getpid(), guard_ = 0;
    ct::FileDescriptor target_, keep_target_;
    bool ready_ = false;
    unsigned attempts_ = 0;

  public:
    Host(std::filesystem::path marker, std::string mode)
        : marker_(std::move(marker)), mode_(std::move(mode)) {
        if (mode_ == "process-loss") {
            int channels[2];
            assert(!pipe2(channels, O_CLOEXEC));
            target_.reset(channels[0]);
            keep_target_.reset(channels[1]);
        } else
            target_.reset(int(syscall(SYS_pidfd_open, getpid(), 0)));
        assert(target_);
    }
    void record(const std::string &value) {
        ct::FileDescriptor file(
            open(marker_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
        assert(file);
        const auto bytes = value + '\n';
        assert(write(file.get(), bytes.data(), bytes.size()) == ssize_t(bytes.size()));
    }
    void guard(pid_t value) {
        guard_ = value;
    }
    int descriptor() const override {
        return target_.get();
    }
    bool start() override {
        record("attempt");
        if (mode_ == "start-failure")
            throw std::runtime_error("injected backend start failure");
        if (mode_ == "busy" && ++attempts_ < 3)
            return false;
        if (mode_ == "slow")
            ct::sleep_for(1.2);
        if (mode_ == "process-loss")
            keep_target_.reset();
        if (mode_ == "guard-death") {
            assert(guard_ > 0 && !kill(guard_, SIGKILL));
            siginfo_t exited{};
            assert(!waitid(P_PID, id_t(guard_), &exited, WEXITED | WNOWAIT));
        }
        record("started");
        return true;
    }
    void renew() override {
        record("renew");
        if (mode_ == "renew-failure")
            throw std::runtime_error("injected backend renewal failure");
    }
    std::vector<ct::json::Value> poll(bool requested) override {
        if (mode_ == "poll-failure")
            throw std::runtime_error("injected backend sample failure");
        record(requested ? "quality-on" : "quality-off");
        std::vector<ct::json::Value> messages;
        if (!ready_) {
            messages.push_back(ct::json::parse("{\"op\":\"native_helper_ready\"}"));
            ready_ = true;
        }
        return messages;
    }
    void restore(const std::function<void()> &pulse = {}) override {
        if (pulse)
            pulse();
        record(getpid() == owner_ ? "parent-restore" : "guard-restore");
        if (mode_ == "restore-failure" && getpid() == owner_)
            throw std::runtime_error("injected parent restoration failure");
    }
    void start(const ct::helper::RecoveryOutput &) override {
        record("guard-start");
    }
    void command(uint8_t value, double) override {
        if (value >= 0x80)
            record("beam-command");
        if (value == 'A')
            record("index-command");
    }
    void poll(double) override {}
    void release_input() override {
        record(getpid() == owner_ ? "parent-release" : "guard-release");
    }
    void restore_tracking() override {
        restore();
    }
};
std::string contents(const std::filesystem::path &path) {
    return std::filesystem::exists(path) ? ct::read_file(path, 131072) : "";
}
void await(const std::filesystem::path &path, const std::string &text, double timeout = 4) {
    const auto deadline = ct::monotonic_time() + timeout;
    while (contents(path).find(text) == std::string::npos) {
        assert(ct::monotonic_time() < deadline);
        ct::sleep_for(.001);
    }
}
void ready(ct::ChildProcess &child) {
    const auto deadline = ct::monotonic_time() + 4;
    do {
        for (const auto &message : child.poll()) {
            const auto operation = ct::json::string(ct::json::field(message.get(), "op"));
            assert(operation != "native_helper_error");
            if (operation == "native_helper_ready")
                return;
        }
        ct::sleep_for(.001);
    } while (ct::monotonic_time() < deadline);
    assert(false);
}
void status(ct::ChildProcess &child, int expected) {
    const auto code = child.exit_status();
    assert(code && WIFEXITED(*code) && WEXITSTATUS(*code) == expected);
}
int run(const std::string &mode, const std::filesystem::path &marker) {
    try {
        Host host(marker, mode);
        ct::helper::RecoveryGuard guard(host);
        host.guard(guard.pid());
        ct::helper::Supervisor supervisor(host, guard);
        supervisor.run();
        return 0;
    } catch (const std::exception &error) {
        ct::helper::RecoveryOutput{}.error(error.what());
        return 1;
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "--helper")
        return run(argv[2], argv[3]);
    char directory[] = "/tmp/controller-utils-supervisor-XXXXXX";
    assert(mkdtemp(directory));
    const auto root = std::filesystem::path(directory);
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    unsigned serial = 0;
    auto marker = [&] { return root / std::to_string(serial++); };
    auto command = [&](const char *mode, const std::filesystem::path &path) {
        return std::vector<std::string>{executable, "--helper", mode, path.string()};
    };
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        const auto path = marker();
        ct::ChildProcess child(command("private", path), root);
        child.send(".");
        ready(child);
        const auto messages = child.close("q");
        assert(!messages.empty());
        assert(ct::json::string(ct::json::field(messages.back().get(), "op")) ==
               "native_helper_restored");
        status(child, 0);
        const auto log = contents(path);
        assert(log.find("parent-restore\n") < log.find("guard-release\n"));
        assert(log.find("guard-restore") == std::string::npos);
    }
    for (const auto *mode : {"busy", "slow"}) {
        const auto path = marker();
        ct::ChildProcess child(command(mode, path), root);
        child.send(".");
        ready(child);
        child.close("q");
        status(child, 0);
        if (std::string(mode) == "busy") {
            const auto log = contents(path);
            size_t count = 0, offset = 0;
            while ((offset = log.find("attempt\n", offset)) != std::string::npos) {
                ++count;
                offset += 8;
            }
            assert(count == 3);
        }
    }
    {
        const auto path = marker();
        ct::ChildProcess child(command("private", path), root);
        ready(child);
        child.send("l");
        await(path, "quality-off\n");
        child.send(std::string("rA") + char(0x80));
        await(path, "beam-command\n");
        await(path, "index-command\n");
        child.close("q");
        status(child, 0);
    }
    for (const auto *mode : {"start-failure", "poll-failure", "process-loss"}) {
        const auto path = marker();
        ct::ChildProcess child(command(mode, path), root);
        await(path, "parent-restore\n");
        const auto messages = child.close("");
        assert(!messages.empty() && ct::json::string(ct::json::field(
                                        messages.back().get(), "op")) == "native_helper_error");
        status(child, 1);
        assert(contents(path).find("guard-release\n") != std::string::npos);
    }
    for (const auto *mode : {"renew-failure", "guard-death", "private"}) {
        const auto path = marker();
        ct::ChildProcess child(command(mode, path), root);
        ready(child);
        const auto input = std::string(mode) == "private" ? "?" : ".";
        child.send(input);
        await(path, "parent-restore\n");
        child.close("");
        status(child, 1);
        if (std::string(mode) == "guard-death")
            assert(contents(path).find("parent-release\n") != std::string::npos);
    }
    {
        const auto path = marker();
        ct::ChildProcess child(command("restore-failure", path), root);
        ready(child);
        child.close("q");
        status(child, 1);
        assert(contents(path).find("guard-restore\n") != std::string::npos);
    }
    for (const auto signal : {SIGTERM, SIGHUP, SIGINT, SIGALRM}) {
        const auto path = marker();
        ct::ChildProcess child(command("private", path), root);
        ready(child);
        assert(!kill(child.pid(), signal));
        await(path, "parent-restore\n");
        child.close("");
        status(child, 1);
    }
    {
        const auto path = marker();
        ct::ChildProcess child(command("private", path), root);
        ready(child);
        const auto before = ct::monotonic_time();
        await(path, "parent-restore\n");
        assert(ct::monotonic_time() - before >= 1.8);
        child.close("");
        status(child, 1);
    }
    for (const char *ending : {"", "?q"}) {
        const auto path = marker();
        ct::ChildProcess child(command("private", path), root);
        child.close(ending);
        status(child, 0);
        assert(contents(path).find("parent-restore\n") != std::string::npos);
    }
    for (const char *argument : {"--invalid", "--private", "--shared"}) {
        const auto helper = std::filesystem::current_path() / "build/controller-utils-helper";
        ct::ChildProcess child({helper.string(), argument}, root);
        child.close("");
        status(child, 1);
    }
    std::filesystem::remove_all(root);
    std::cout << "native supervisor: 100 sessions, startup, commands, timeout, signals, backend "
                 "failures and guard recovery passed\n";
}
