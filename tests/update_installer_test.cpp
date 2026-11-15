#include "update/installer.h"
#include "update/repair.h"
#include "arm_platform.h"
#include "update_fixture.h"
#include "app/settings.h"
#include <grp.h>
#include <iostream>
#include <sys/wait.h>

extern "C" int __real_renameat2(int, const char *, int, const char *, unsigned);
extern "C" int __real_unlinkat(int, const char *, int);
namespace {
pid_t fault_process = 0;
unsigned crash_at = 0;
bool crash_before = false, crash_after_application = false;
unsigned swaps = 0;
using namespace update_fixture;
void owned(const std::filesystem::path &path, uid_t user) {
    assert(!chown(path.c_str(), user, user));
    for (const auto &entry : std::filesystem::recursive_directory_iterator(path))
        assert(!chown(entry.path().c_str(), user, user));
}
template <class Function> void rejects(Function fn) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
struct Fixture {
    std::filesystem::path root, base, app, data, helpers, cache;
    Key key = keypair();
    j::Value expected;
    u::UpdateInstallation installation;
    Fixture(const std::filesystem::path &path)
        : root(path), base(path / "user"), app(base / "app"), data(base / "app-data"),
          helpers(path / "native"), cache(path / "updates"),
          installation{{app, helpers}, data, cache, 1000, 1000} {
        std::filesystem::create_directories(app / "assets");
        std::filesystem::create_directories(data);
        std::filesystem::create_directories(helpers);
        std::filesystem::create_directory(cache);
        assert(!chmod(cache.c_str(), 0700));
        ct::atomic_write(cache / "public.pem", public_key(key.get()));
        ct::atomic_write(app / "release.json",
                         R"({"version":"0.1.0","sequence":1,"platform":"linuxarm64"})");
        ct::atomic_write(app / "controller-utils", "previous application", 0755);
        ct::atomic_write(app / "assets/update-public.pem", public_key(key.get()), 0644);
        ct::atomic_write(app / "assets/update-channel.json",
                         std::string("{\"url\":\"") + u::GitHubChannel + "\"}", 0644);
        ct::atomic_write(app / "assets/tracking-calibration.json", "private calibration", 0644);
        ct::atomic_write(data / "app.lock", "");
        ct::save_settings(data / "settings.json", {});
        for (const auto &name : u::helper_files(ct::TrackingBackend::Tracker))
            ct::atomic_write(helpers / name, "previous " + name, 0644);
        for (const auto name : u::DriverFiles)
            ct::atomic_write(app / "native-driver" / name, "previous driver", 0644);
        std::filesystem::create_directory_symlink("app/native-driver", base / "native-driver");
        for (size_t index = 0; index < u::RuntimeFiles.size(); ++index) {
            auto path = root / "runtime" / u::RuntimeFiles[index];
            ct::atomic_write(path, "runtime", 0644);
            installation.components.runtime[index] = path;
        }
        expected =
            u::capture_requirements(installation.components, ct::TrackingBackend::Tracker).value;
        pending(2);
        assert(!chmod((helpers / "controller-utils-updater").c_str(), 0755));
        auto old = j::parse(ct::read_file(data / "updates/pending/latest.json", u::MaxMetadata));
        json_object_object_add(old.get(), "version", json_object_new_string("0.1.0"));
        json_object_object_add(old.get(), "sequence", json_object_new_int(1));
        json_object_object_add(
            j::field(j::field(old.get(), "requirements"), "helper"), "controller-utils-updater",
            json_object_new_string(ct::sha256(helpers / "controller-utils-updater").c_str()));
        const auto old_bytes = j::encode(old.get());
        sign(cache / "old.sig", key.get(), old_bytes);
        u::record_repair_evidence(ct::ProtectedDirectory(helpers), old_bytes,
                                  ct::read_file(cache / "old.sig", 64), public_key(key.get()),
                                  {"0.1.0", 1});
    }
    void pending(unsigned sequence) {
        auto entries = payload();
        const auto digest = ct::sha256_bytes(program().data(), program().size());
        for (const auto field : {"driver", "helper"}) {
            auto files = j::field(expected.get(), field);
            json_object_object_foreach(files, name, value) {
                (void)value;
                json_object_object_add(files, name, json_object_new_string(digest.c_str()));
            }
        }
        const u::Release version{"0.1." + std::to_string(sequence - 1), sequence};
        for (auto &entry : entries) {
            if (entry.name == "release.json")
                entry.data = j::encode(version.to_json().get());
            if (entry.name == "assets/update-public.pem")
                entry.data = public_key(key.get());
            if (entry.name == "assets/update-channel.json")
                entry.data = std::string("{\"url\":\"") + u::GitHubChannel + "\"}";
        }
        const auto pending = data / "updates/pending";
        std::filesystem::create_directories(pending);
        auto archive = make_archive(pending / "controller-utils-test-linuxarm64.tar.gz", entries);
        auto metadata = version.to_json();
        json_object_object_add(metadata.get(), "schema", json_object_new_int(5));
        json_object_object_add(metadata.get(), "expires", json_object_new_int(0));
        json_object_object_add(metadata.get(), "artifact",
                               json_object_new_string(archive.artifact.c_str()));
        json_object_object_add(metadata.get(), "size", json_object_new_uint64(archive.size));
        json_object_object_add(metadata.get(), "sha256",
                               json_object_new_string(archive.sha256.c_str()));
        json_object_object_add(metadata.get(), "notes", json_object_new_string("test"));
        json_object_object_add(metadata.get(), "requirements", json_object_get(expected.get()));
        ct::atomic_write(pending / "latest.json", j::encode(metadata.get()));
        sign(pending / "latest.sig", key.get(), j::encode(metadata.get()));
        owned(base, 1000);
    }
    void apply() {
        u::install_update(installation, false, [] { return false; });
    }
    void confirm() {
        const auto pid = fork();
        assert(pid >= 0);
        if (!pid) {
            assert(!setgroups(0, nullptr) && !setgid(1000) && !setuid(1000));
            ct::ApplicationLock lock(data / "app.lock");
            u::confirm(app, data, lock);
            _exit(0);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid && status == 0);
        u::install_update(installation, true);
    }
    void previous() {
        assert(u::release(app).sequence == 1);
        assert(ct::read_file(helpers / "controller-utils-helper", 128) ==
               "previous controller-utils-helper");
        assert(ct::read_file(app / "assets/tracking-calibration.json", 128) ==
               "private calibration");
        assert(u::verify_repair_updater(ct::ProtectedDirectory(helpers), public_key(key.get()))
                   .sequence == 1);
    }
};
} // namespace
extern "C" int __wrap_renameat2(int from, const char *source, int to, const char *destination,
                                unsigned flags) {
    const bool fault = getpid() == fault_process && ++swaps == crash_at;
    if (fault && crash_before)
        _exit(77);
    const auto result = __real_renameat2(from, source, to, destination, flags);
    if (fault && !result)
        _exit(77);
    return result;
}
extern "C" int __wrap_unlinkat(int directory, const char *name, int flags) {
    if (getpid() == fault_process && crash_after_application &&
        !std::strcmp(name, "archive.tar.gz"))
        _exit(77);
    return __real_unlinkat(directory, name, flags);
}
int main() {
    if (getuid()) {
        rejects([] { u::install_update({}, false); });
        std::cout << "Native installer rejects unprivileged callers. Privileged cycle runs in "
                     "release CI.\n";
        return 0;
    }
    const auto root = std::filesystem::path("/var/lib") /
                      ("controller-utils-update-test-" + std::to_string(getpid()));
    assert(std::filesystem::create_directory(root));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    {
        Fixture fixture(root / "healthy");
        u::install_update(fixture.installation, false, [] { return true; });
        fixture.previous();
        fixture.apply();
        assert(u::release(fixture.app).sequence == 2);
        assert(u::verify_repair_updater(ct::ProtectedDirectory(fixture.helpers),
                                        public_key(fixture.key.get()))
                   .sequence == 2);
        assert(ct::sha256(fixture.helpers / "controller-utils-helper") ==
               ct::sha256_bytes(program().data(), program().size()));
        assert(ct::sha256(fixture.base / "native-driver" / u::DriverFiles[1]) ==
               ct::sha256_bytes(program().data(), program().size()));
        fixture.confirm();
        assert(u::verify_repair_updater(ct::ProtectedDirectory(fixture.helpers),
                                        public_key(fixture.key.get()))
                   .sequence == 2);
        fixture.apply();
        assert(u::release(fixture.app).sequence == 2);
        fixture.pending(3);
        ct::atomic_write(fixture.app / "assets/update-public.pem", "invalid key", 0644);
        owned(fixture.base, 1000);
        rejects([&] { fixture.apply(); });
        assert(u::release(fixture.app).sequence == 2);
        assert(ct::sha256(fixture.helpers / "controller-utils-helper") ==
               ct::sha256_bytes(program().data(), program().size()));
        assert(u::verify_repair_updater(ct::ProtectedDirectory(fixture.helpers),
                                        public_key(fixture.key.get()))
                   .sequence == 2);
    }
    {
        Fixture fixture(root / "rollback");
        fixture.apply();
        fixture.apply();
        fixture.previous();
    }
    {
        Fixture fixture(root / "partial-confirmation");
        fixture.apply();
        rejects([&] { u::install_update(fixture.installation, true); });
        fixture.apply();
        fixture.previous();
    }
    {
        Fixture fixture(root / "tamper");
        ct::atomic_write(fixture.data / "updates/pending/latest.sig", std::string(64, 'x'));
        owned(fixture.base, 1000);
        rejects([&] { fixture.apply(); });
        fixture.previous();
    }
    for (unsigned fault = 1; fault <= 5; ++fault) {
        Fixture fixture(root / ("interrupted-" + std::to_string(fault)));
        const auto child = fork();
        assert(child >= 0);
        if (!child) {
            fault_process = getpid();
            crash_at = (fault + 1) / 2;
            crash_before = fault % 2;
            crash_after_application = fault == 5;
            if (fault == 5)
                crash_at = 0;
            fixture.apply();
            _exit(88);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 77);
        std::filesystem::remove_all(fixture.data / "updates/pending");
        fixture.apply();
        fixture.previous();
    }
    std::cout << "Privileged updater: runtime deferral, complete install, startup confirmation, "
                 "rollback and signature rejection passed\n";
}
