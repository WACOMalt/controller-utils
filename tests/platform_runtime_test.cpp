#include "platform/linux/files.h"
#include "app/settings.h"
#include "platform/linux/resource.h"
#include "steamvr/runtime.h"
#include "steamvr/module.h"
#include "steamvr/shortcuts.h"
#include <cassert>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>

namespace {
template <class Function> void rejects(Function function, const std::string &message = "") {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &error) {
        rejected = std::string(error.what()).find(message) != std::string::npos;
    }
    assert(rejected);
}
} // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("controller-utils-platform-" + std::to_string(getpid()));
    if (!std::filesystem::create_directory(root))
        throw std::runtime_error("test directory already exists");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    const auto settings = root / "data/settings.json";
    ct::atomic_write(settings, "abc");
    assert(ct::read_file(settings, 3) == "abc");
    assert(ct::sha256_bytes("abc", 3) == ct::sha256(settings));
    assert(ct::sha256_bytes(nullptr, 0) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    rejects([] { ct::sha256_bytes(nullptr, 1); }, "invalid digest");
    assert(ct::sha256(settings) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert((std::filesystem::status(settings).permissions() & std::filesystem::perms::all) ==
           (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
    const auto saved = root / "app/settings.json";
    auto options = ct::load_settings(saved);
    assert(options.tracking_enabled && options.pointer_buttons == 0x80);
    assert(options.steam_shortcuts);
    assert(options.smoothing_ms == 20);
    options.brightness = 60;
    options.tracking_enabled = false;
    options.steam_shortcuts = false;
    ct::save_settings(saved, options);
    const auto contents = ct::read_file(saved, 131072);
    auto loaded = ct::load_settings(saved);
    assert(loaded.brightness == 60 && !loaded.tracking_enabled);
    assert(!loaded.steam_shortcuts);
    options.smoothing_ms = 151;
    rejects([&] { ct::save_settings(saved, options); }, "invalid smoothing_ms");
    assert(ct::read_file(saved, 131072) == contents);
    ct::atomic_write(saved, R"({"brightness":80,"smoothing_ms":40})");
    loaded = ct::load_settings(saved);
    assert(loaded.brightness == 80);
    assert(loaded.steam_shortcuts);
    assert(loaded.smoothing_ms == 40);
    ct::atomic_write(saved, "corrupt settings");
    rejects([&] { ct::load_settings(saved); }, "invalid JSON");
    assert(ct::read_file(saved, 131072) == "corrupt settings");
    ct::atomic_write(saved, std::string(131073, ' '));
    rejects([&] { ct::load_settings(saved); }, "size limit");
    const auto fifo = root / "unexpected-fifo";
    assert(mkfifo(fifo.c_str(), 0600) == 0);
    const auto child = fork();
    assert(child >= 0);
    if (!child) {
        alarm(1);
        rejects([&] { ct::read_file(fifo, 100); }, "regular file");
        _exit(0);
    }
    int child_status = 0;
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    rejects([&] { ct::read_file(settings, 2); }, "size limit");
    ct::atomic_write(settings, "replacement", 0640);
    assert(ct::read_file(settings, 100) == "replacement");
    std::filesystem::create_directory(root / "occupied");
    rejects([&] { ct::atomic_write(root / "occupied", "must not replace a directory"); });
    assert(std::filesystem::is_directory(root / "occupied"));
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
        assert(entry.path().filename().string().find(".pending-") == std::string::npos);
    rejects([&] { ct::read_file(root, 100); }, "regular file");
    const auto mono = ct::monotonic_time(), raw = ct::raw_time();
    assert(mono > 0 && raw > 0);
    ct::sleep_for(.001);
    assert(ct::monotonic_time() > mono && ct::raw_time() > raw);
    rejects([&] { ct::sleep_for(-1); });
    rejects([&] { ct::Runtime runtime(false, "/missing/controller-utils-openvr.so"); },
            "unavailable");
    const auto library = std::filesystem::absolute("build/fake_openvr.so").string();
    void *handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(handle);
    const auto reset = reinterpret_cast<void (*)()>(dlsym(handle, "ct_test_reset"));
    const auto initialized = reinterpret_cast<unsigned (*)()>(dlsym(handle, "ct_test_initialized"));
    const auto shutdown = reinterpret_cast<unsigned (*)()>(dlsym(handle, "ct_test_shutdown"));
    const auto type = reinterpret_cast<unsigned (*)()>(dlsym(handle, "ct_test_application_type"));
    assert(reset && initialized && shutdown && type);
    const auto reset_address = reinterpret_cast<const void *>(reset);
    const auto module = ct::steamvr::Module::loaded(reset_address, library.c_str());
    assert(module.contains(reset_address, 1, true));
    rejects([&] { ct::steamvr::Module::loaded(reset_address, "/wrong/fake_openvr.so"); });
    reset();
    const auto incompatible = root / "drivers/cv/bin/linuxarm64/XRService";
    ct::atomic_write(incompatible, "unsupported private queue ABI");
    rejects([&] { ct::check_runtime(root.string()); }, "XRService changed");
    assert(!initialized() && !shutdown());
    assert(ct::read_file(incompatible, 100) == "unsupported private queue ABI");
    {
        ct::Runtime runtime(false, library);
        assert(initialized() == 1 && !shutdown() && type() == 3);
        rejects([&] { runtime.queues(); }, "FnTable:IVRBlockQueue_005");
        rejects([&] { runtime.path_handle("/user/head"); }, "FnTable:IVRPaths_002");
        rejects([&] { runtime.path_handle(""); }, "empty");
        runtime.close();
        runtime.close();
        assert(shutdown() == 1);
        rejects([&] { runtime.head_pose(); }, "closed");
        rejects([&] { runtime.queues(); }, "closed");
        rejects([&] { runtime.path_handle("/user/head"); }, "closed");
    }
    reset();
    {
        ct::Runtime runtime(true, library);
        assert(type() == 2 && runtime.overlay);
    }
    assert(initialized() == 1 && shutdown() == 1);
    reset();
    setenv("CT_TEST_OPENVR_INIT_FAILURE", "1", 1);
    rejects([&] { ct::Runtime runtime(false, library); }, "OpenVR background init: 100");
    unsetenv("CT_TEST_OPENVR_INIT_FAILURE");
    assert(initialized() == 1 && !shutdown());
    reset();
    setenv("CT_TEST_OPENVR_INTERFACE_FAILURE", "1", 1);
    rejects([&] { ct::Runtime runtime(false, library); }, "IVRSystem_026: 105");
    unsetenv("CT_TEST_OPENVR_INTERFACE_FAILURE");
    assert(initialized() == 1 && shutdown() == 1);
    dlclose(handle);
    std::cout << "platform/runtime: atomic data, hashing, bounded IO and OpenVR ownership passed\n";
}
