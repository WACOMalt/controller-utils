#include "update/setup.h"
#include "update/repair.h"
#include "update/maintenance.h"
#include "update_fixture.h"
#include "arm_platform.h"
#include <iostream>
#include <grp.h>
#include <sys/wait.h>

namespace {
using namespace update_fixture;
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
struct Fixture {
    std::filesystem::path root, feed;
    Key key = keypair();
    std::string public_pem = public_key(key.get());
    u::UpdateInstallation installation;
    u::SetupPolicies policies;
    explicit Fixture(const std::filesystem::path &directory)
        : root(directory), feed(root / "feed"), installation{{root / "user/app", root / "native"},
                                                             root / "user/app-data",
                                                             root / "updates",
                                                             1000,
                                                             1000},
          policies{root / "policies/sudoers", root / "policies/udev.rules",
                   root / "policies/controller-utils-maintenance.service",
                   root / "policies/controller-utils.conf"} {
        std::filesystem::create_directories(feed);
        std::filesystem::create_directory(root / "policies");
        auto entries = payload();
        for (auto &entry : entries) {
            if (entry.name == "assets/update-public.pem")
                entry.data = public_pem;
            if (entry.name == "assets/update-channel.json")
                entry.data = std::string("{\"url\":\"") + u::GitHubChannel + "\"}";
        }
        auto expected = requirements();
        struct utsname platform{};
        assert(!uname(&platform));

        for (size_t index = 0; index < u::RuntimeFiles.size(); ++index) {
            auto path = root / "runtime" / u::RuntimeFiles[index];
            ct::atomic_write(path, "runtime", 0644);
            installation.components.runtime[index] = path;
            json_object_object_add(j::field(expected.get(), "runtime"), u::RuntimeFiles[index],
                                   json_object_new_string(ct::sha256(path).c_str()));
        }
        auto archive = make_archive(feed / "controller-utils-fixture-linuxarm64.tar.gz", entries);
        auto metadata = archive.release.to_json();
        json_object_object_add(metadata.get(), "schema", json_object_new_int(5));
        json_object_object_add(metadata.get(), "expires", json_object_new_int(0));
        json_object_object_add(metadata.get(), "artifact",
                               json_object_new_string(archive.artifact.c_str()));
        json_object_object_add(metadata.get(), "size", json_object_new_uint64(archive.size));
        json_object_object_add(metadata.get(), "sha256",
                               json_object_new_string(archive.sha256.c_str()));
        json_object_object_add(metadata.get(), "notes", json_object_new_string("fixture"));
        json_object_object_add(metadata.get(), "requirements", expected.release());
        const auto bytes = j::encode(metadata.get());
        ct::atomic_write(feed / "latest.json", bytes);
        sign(feed / "latest.sig", key.get(), bytes);
    }
    void install() {
        u::setup_install(installation, policies, feed, public_pem, [] { return false; });
    }
    void remove() {
        u::setup_remove(installation, policies, public_pem, [] { return false; });
    }
    ~Fixture() {
        std::filesystem::remove_all(root);
    }
};
} // namespace
int main() {
    if (getuid()) {
        rejects([] { u::setup_install({}, {}, "/missing", "", [] { return false; }); });
        rejects([] { u::setup_remove({}, {}, "", [] { return false; }); });
        std::cout << "Fresh setup rejects unprivileged root operations\n";
        return 0;
    }
    const auto root = std::filesystem::path("/var/lib") /
                      ("controller-utils-setup-test-" + std::to_string(getpid()));
    {
        Fixture f(root);
        rejects([&] {
            u::setup_install(f.installation, f.policies, f.feed, f.public_pem, [] { return true; });
        });
        assert(!std::filesystem::exists(f.installation.cache));
        f.install();
        const auto enabled = f.policies.service.parent_path() / "multi-user.target.wants" /
                             f.policies.service.filename();
        assert(ct::read_file(f.policies.service, 16384) == u::MaintenanceService);
        assert(ct::read_file(f.policies.keep, 16384) == u::MaintenanceKeep);
        assert(std::filesystem::read_symlink(enabled) ==
               "../" + f.policies.service.filename().string());
        {
            ct::ProtectedDirectory native(f.installation.components.helpers);
            assert(u::verify_repair_updater(native, f.public_pem).sequence == 2);
            assert(u::inspect_repair(f.installation, f.policies, f.public_pem) ==
                   u::RepairStatus::Intact);
            std::filesystem::remove(f.policies.sudoers);
            assert(u::inspect_repair(f.installation, f.policies, f.public_pem) ==
                   u::RepairStatus::Missing);
            u::repair_permissions(f.installation, f.policies, f.public_pem, [] {});
            assert(u::inspect_repair(f.installation, f.policies, f.public_pem) ==
                   u::RepairStatus::Intact);
        }
        f.install();
        assert(ct::read_file(f.policies.sudoers, 16384) == "package fixture");
        struct stat info{};
        assert(!stat(f.installation.components.helpers.c_str(), &info) && info.st_uid == 0 &&
               !(info.st_mode & 0022));
        const auto child = fork();
        assert(child >= 0);
        if (!child) {
            assert(!setgroups(0, nullptr) && !setgid(1000) && !setuid(1000));
            const auto target = f.installation.components.helpers / "controller-utils-updater";
            assert(access(target.c_str(), W_OK) != 0);
            assert(unlink(target.c_str()) != 0);
            _exit(0);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child && status == 0);
        rejects([&] {
            u::setup_remove(f.installation, f.policies, f.public_pem, [] { return true; });
        });
        f.remove();
        assert(!std::filesystem::exists(f.installation.components.helpers));
        assert(!std::filesystem::exists(f.installation.cache));
        assert(!std::filesystem::exists(f.policies.sudoers) &&
               !std::filesystem::exists(f.policies.udev));
        assert(!std::filesystem::exists(f.policies.service) &&
               !std::filesystem::exists(f.policies.keep) && !std::filesystem::is_symlink(enabled));
    }
    for (unsigned invalid = 0; invalid < 10; ++invalid) {
        Fixture f(root);
        if (invalid == 0)
            ct::atomic_write(f.feed / "latest.sig", std::string(64, 'x'));
        if (invalid == 1)
            ct::atomic_write(f.installation.components.runtime[0], "wrong runtime");
        if (invalid == 2)
            ct::atomic_write(f.policies.udev, "unrelated policy", 0644);
        if (invalid == 3)
            std::filesystem::create_directory_symlink(f.root / "feed",
                                                      f.installation.components.helpers);
        if (invalid == 4) {
            std::filesystem::create_directory(f.installation.cache);
            assert(!chown(f.installation.cache.c_str(), 1000, 1000));
        }
        if (invalid == 5) {
            std::filesystem::create_directory(f.installation.components.helpers);
            ct::atomic_write(f.installation.components.helpers / "unrelated", "preserve");
        }
        if (invalid == 6)
            ct::atomic_write(f.policies.service, "administrator service", 0644);
        if (invalid == 7)
            ct::atomic_write(f.policies.keep, "administrator keep list", 0644);
        if (invalid == 8)
            std::filesystem::create_directory_symlink(f.feed, f.policies.service.parent_path() /
                                                                  "multi-user.target.wants");
        if (invalid == 9) {
            const auto wants = f.policies.service.parent_path() / "multi-user.target.wants";
            std::filesystem::create_directory(wants);
            std::filesystem::create_symlink("/dev/null", wants / f.policies.service.filename());
        }
        rejects([&] { f.install(); });
        assert(!std::filesystem::exists(f.policies.sudoers));
        if (invalid == 2)
            assert(ct::read_file(f.policies.udev, 16384) == "unrelated policy");
        if (invalid == 6)
            assert(ct::read_file(f.policies.service, 16384) == "administrator service");
        if (invalid == 7)
            assert(ct::read_file(f.policies.keep, 16384) == "administrator keep list");
    }
    {
        Fixture f(root);
        f.install();
        ct::atomic_write(f.policies.keep, "administrator keep list", 0644);
        rejects([&] { f.remove(); });
        assert(std::filesystem::exists(f.policies.sudoers));
        ct::atomic_write(f.policies.keep, u::MaintenanceKeep, 0644);
        ct::atomic_write(f.policies.udev, "changed policy", 0644);
        rejects([&] { f.remove(); });
        assert(std::filesystem::exists(f.policies.sudoers));
        ct::atomic_write(f.policies.udev, "package fixture", 0644);
        ct::atomic_write(f.installation.components.helpers / "controller-utils-helper",
                         "changed helper", 0755);
        rejects([&] { f.install(); });
        f.remove();
    }
    std::cout << "Fresh signed setup: install, repeat, runtime pins, protected policies, privilege "
                 "separation, tamper rejection and removal passed\n";
}
