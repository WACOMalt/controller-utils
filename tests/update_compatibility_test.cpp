#include "update/compatibility.h"
#include "platform/linux/files.h"
#include <cassert>
#include <iostream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {
namespace u = ct::update;
namespace j = ct::json;
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace
int main() {
    const auto root = std::filesystem::current_path() / "build" /
                      ("update-compatibility-" + std::to_string(getpid()));
    assert(std::filesystem::create_directory(root));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    const auto selector = root / "backend-selection";
    std::filesystem::create_directory(selector);
    rejects([&] { ct::installed_backend(selector); });
    std::filesystem::create_directory(selector / "native-tracker");
    assert(ct::installed_backend(selector) == ct::TrackingBackend::Tracker);
    std::filesystem::remove(selector / "native-tracker");
    std::filesystem::create_directory_symlink(root, selector / "native-tracker");
    rejects([&] { ct::installed_backend(selector); });
    rejects([&] { ct::installed_backend("relative"); });
    ct::atomic_write(root / "invalid-helper-root", "file");
    rejects([&] { ct::installed_backend(root / "invalid-helper-root"); });
    u::Installation installation{root / "app", root / "helpers"};
    for (size_t i = 0; i < installation.runtime.size(); ++i)
        installation.runtime[i] = root / "runtime" / u::RuntimeFiles[i];
    for (const auto &name : u::helper_files(ct::TrackingBackend::Tracker))
        ct::atomic_write(installation.helpers / name, "helper " + name);
    for (const auto name : u::DriverFiles)
        ct::atomic_write(root / "native-driver" / name, "driver " + std::string(name));
    for (const auto &path : installation.runtime)
        ct::atomic_write(path, "runtime " + path.filename().string());
    struct utsname platform{};
    assert(!uname(&platform));
    auto tracker = u::capture_requirements(installation, ct::TrackingBackend::Tracker);
    assert(tracker.machine == platform.machine);
    assert(j::string(j::field(tracker.value.get(), "backend")) == "native-tracker");
    assert(json_object_object_length(j::field(tracker.value.get(), "helper")) == 6);
    assert(j::string(j::field(j::field(tracker.value.get(), "runtime"), "XRService")) ==
           ct::sha256(installation.runtime[0]));
    tracker.machine = "aarch64";
    u::check_requirements(tracker.value.get(), tracker);
    tracker.machine = "x86_64";
    rejects([&] { u::check_requirements(tracker.value.get(), tracker); });
    tracker.machine = "aarch64";
    for (const auto &path :
         {root / "native-driver" / u::DriverFiles[1],
          installation.helpers / "controller-utils-helper",
          installation.helpers / "native-tracker/loader.bin", installation.runtime[0]}) {
        const auto contents = ct::read_file(path, 4096);
        ct::atomic_write(path, contents + " changed");
        auto changed = u::capture_requirements(installation, ct::TrackingBackend::Tracker);
        changed.machine = "aarch64";
        if (path == installation.runtime[0])
            rejects([&] { u::check_requirements(tracker.value.get(), changed); });
        else
            u::check_requirements(tracker.value.get(), changed);
        ct::atomic_write(path, contents);
    }
    for (const auto name : {"driver_cv.so", "vrclient.so", "vrcompositor", "steamwebhelper"}) {
        const auto path = root / "runtime" / name;
        ct::atomic_write(path, "unrelated vendor release");
        auto changed = u::capture_requirements(installation, ct::TrackingBackend::Tracker);
        changed.machine = "aarch64";
        u::check_requirements(tracker.value.get(), changed);
        std::filesystem::remove(path);
        auto missing = u::capture_requirements(installation, ct::TrackingBackend::Tracker);
        missing.machine = "aarch64";
        u::check_requirements(tracker.value.get(), missing);
    }
    auto path = installation.runtime[0];
    const auto contents = ct::read_file(path, 4096);
    const auto real = path.string() + ".real";
    std::filesystem::rename(path, real);
    std::filesystem::create_symlink(real, path);
    auto linked = u::capture_requirements(installation, ct::TrackingBackend::Tracker);
    linked.machine = "aarch64";
    u::check_requirements(tracker.value.get(), linked);
    std::filesystem::remove(path);
    rejects([&] { u::capture_requirements(installation, ct::TrackingBackend::Tracker); });
    assert(!mkfifo(path.c_str(), 0600));
    rejects([&] { u::capture_requirements(installation, ct::TrackingBackend::Tracker); });
    std::filesystem::remove(path);
    ct::atomic_write(path, contents);
    assert(!truncate(path.c_str(), 512 * 1024 * 1024 + 1));
    rejects([&] { u::capture_requirements(installation, ct::TrackingBackend::Tracker); });
    installation.runtime[0] = std::filesystem::path("relative");
    rejects([&] { u::capture_requirements(installation, ct::TrackingBackend::Tracker); });
    std::cout << "native installed-component and active-backend update checks passed\n";
}
