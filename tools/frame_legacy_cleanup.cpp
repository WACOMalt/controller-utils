#include "frame_migration_policies.h"
#include "platform/linux/files.h"
#include "platform/linux/protected_directory.h"
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr char previous_updater[] = "controller-utils-updater";
constexpr char previous_digest[] =
    "766282dcc07fbe55dcbaeb1a52c94d6619c388d4359f8eef9cbcb84fed3d09bb";
const std::filesystem::path Keep = "/etc/atomic-update.conf.d/controller-utils-frame.conf";
const std::filesystem::path Rule = "/etc/udev/rules.d/70-controller-utils.rules";

ct::FileDescriptor verified_updater() {
    const ct::ProtectedDirectory backup("/var/lib/controller-utils-updates/previous-native");
    auto binary = backup.file(previous_updater, 16 * 1024 * 1024);
    const auto bytes = ct::read_file(binary.get(), 16 * 1024 * 1024);
    if (ct::sha256_bytes(bytes.data(), bytes.size()) != previous_digest)
        throw std::runtime_error("The previous updater is not the published v0.2.0 updater");
    backup.verify_file(previous_updater, binary.get());
    return binary;
}

bool missing_policy(const std::filesystem::path &path, const std::string &contents) {
    const ct::ProtectedDirectory directory(path.parent_path());
    try {
        auto file = directory.file(path.filename().string(), 16384);
        if (ct::read_file(file.get(), 16384) != contents)
            throw std::runtime_error("Existing policy differs at " + path.string());
        directory.verify_file(path.filename().string(), file.get());
    } catch (const std::system_error &error) {
        if (error.code().value() == ENOENT)
            return true;
        throw;
    }
    return false;
}

void wait_for(pid_t child) {
    if (child < 0)
        throw std::system_error(errno, std::generic_category(), "start legacy cleanup");
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "wait for legacy cleanup");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        throw std::runtime_error("Legacy cleanup command failed");
}

char *const environment[] = {nullptr};

[[noreturn]] void previous(int fd, const char *operation) {
    char *const argv[] = {const_cast<char *>(previous_updater), const_cast<char *>(operation),
                          nullptr};
    fexecve(fd, argv, environment);
    throw std::system_error(errno, std::generic_category(), "run the original updater");
}

void reload_udev(const char *action, const char *option) {
    auto child = fork();
    if (child == 0) {
        execle("/usr/bin/udevadm", "udevadm", action, option, nullptr, environment);
        _exit(127);
    }
    wait_for(child);
}

void finish(int fd) {
    const auto needs_keep = missing_policy(Keep, frame_keep_policy);
    (void)missing_policy(Rule, frame_udev_policy);
    const auto child = fork();
    if (child == 0) {
        try {
            previous(fd, "--uninstall");
        } catch (...) {
            _exit(127);
        }
    }
    wait_for(child);
    if (missing_policy(Rule, frame_udev_policy))
        ct::atomic_write(Rule, frame_udev_policy, 0644);
    if (needs_keep)
        ct::atomic_write(Keep, frame_keep_policy, 0644);
    reload_udev("control", "--reload-rules");
    reload_udev("trigger", "--subsystem-match=hidraw");
}
} // namespace

int main(int argc, char **argv) {
    try {
        const auto caller = std::getenv("SUDO_UID");
        if (getuid() != 0 || geteuid() != 0 || argc != 2 ||
            (caller && std::string(caller) != "1000"))
            throw std::runtime_error("Unexpected legacy migration authorization");
        const std::string operation = argv[1];
        if (operation == "--repair")
            return 0;
        if (operation != "--repair-status" && operation != "--apply" && operation != "--confirm" &&
            operation != "--uninstall")
            throw std::runtime_error("Unknown legacy migration operation");
        const auto original = verified_updater();
        if (operation == "--repair-status")
            std::cout << "controller-utils-frame-migration-v1\n";
        else if (operation == "--confirm")
            finish(original.get());
        else
            previous(original.get(), argv[1]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Controller Utils legacy migration: " << error.what() << '\n';
        return 1;
    }
}
