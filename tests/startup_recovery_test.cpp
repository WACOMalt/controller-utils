#include "steamvr/startup_recovery.h"
#include <cassert>
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>

int main() {
    const auto directory = std::filesystem::current_path() / "build" /
                           ("startup-recovery-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{directory};
    const std::string a = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    const std::string b = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    ct::StartupRecovery first(directory / "state", a), rebooted(directory / "state", b);
    auto crashed = [](ct::StartupRecovery &recovery) {
        const pid_t pid = fork();
        assert(pid >= 0);
        if (!pid)
            _exit(recovery.begin() ? 0 : 1);
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    };
    crashed(first);
    crashed(first);
    assert(!first.begin() && first.disabled());
    assert(!rebooted.begin() && rebooted.disabled());
    rebooted.retry();
    assert(rebooted.disabled());
    assert(rebooted.begin() && !rebooted.disabled());
    bool concurrent_rejected = false;
    try {
        rebooted.begin();
    } catch (const std::exception &) {
        concurrent_rejected = true;
    }
    assert(concurrent_rejected);
    const auto previous = rebooted;
    rebooted.clean();
    assert(rebooted.begin());
    previous.healthy();
    bool stale_writer_rejected = false;
    try {
        rebooted.begin();
    } catch (const std::exception &) {
        stale_writer_rejected = true;
    }
    assert(stale_writer_rejected);
    rebooted.healthy();
    assert(rebooted.begin());
    rebooted.clean();
    crashed(first);
    crashed(first);
    assert(!first.begin());
    first.retry();
    crashed(first);
    assert(rebooted.begin());
    rebooted.clean();
    ct::atomic_write(directory / "state", "broken");
    bool rejected = false;
    try {
        first.begin();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
    first.fail();
    assert(first.disabled() && first.failed() && !first.begin());
    first.retry();
    assert(first.begin());
    first.clean();
    std::cout << "startup recovery: failure threshold, restart, boot and corruption passed\n";
}
