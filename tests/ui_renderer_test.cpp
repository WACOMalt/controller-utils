#include "ui/renderer.h"
#include "platform/linux/resource.h"
#include "ui/button_glyphs.h"
#include <fontconfig/fontconfig.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <thread>

namespace {
template <class T> T symbol(void *library, const char *name) {
    const auto address = dlsym(library, name);
    assert(address);
    return reinterpret_cast<T>(address);
}
size_t descriptors() {
    size_t result = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++result;
    }
    return result;
}
} // namespace
int main() {
    const auto root = std::filesystem::current_path();
    const auto library = root / "build/fake_openvr.so";
    void *module = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(module);
    const auto reset = symbol<void (*)()>(module, "ct_test_reset");
    const auto initialized = symbol<unsigned (*)()>(module, "ct_test_initialized");
    const auto shutdowns = symbol<unsigned (*)()>(module, "ct_test_shutdown");
    const auto created = symbol<unsigned (*)()>(module, "ct_test_overlays_created");
    const auto destroyed = symbol<unsigned (*)()>(module, "ct_test_overlays_destroyed");
    const auto submitted = symbol<unsigned (*)()>(module, "ct_test_textures_submitted");
    const auto shown = symbol<unsigned (*)()>(module, "ct_test_overlays_shown");
    assert(!setenv("CT_TEST_OPENVR_HEAD_VALID", "1", 1));
    size_t before = 0;
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        reset();
        {
            ct::Runtime runtime(true, library);
            ct::ui::Presentation presentation;
            ct::ui::Renderer renderer(runtime, presentation, root);
            assert(initialized() == 1);
            presentation.send(ct::ui::Operation::ShowSettings);
            const auto now = ct::raw_time();
            presentation.send(ct::ui::Controls{now,
                                               1 | 0x02200000,
                                               {.2f, .5f},
                                               {.1f, .2f},
                                               {-.2f, .3f},
                                               {-.4f, .3f, 0},
                                               {.5f, -.25f, 0}});
            presentation.send(ct::ui::KeyboardTyping{bool(cycle % 2)});
            presentation.send(ct::ui::Feedback{true, "realign", "View + Menu", now, .5});
            ct::BodyPose pose;
            pose.optical_time = pose.orientation_time = pose.position_time = now;
            pose.model = {1, 0, 0, 0, 0, .8660254f, .5f, 0, 0, -.5f, .8660254f, 0, 0, 0, -.4f, 1};
            presentation.send(pose);
            std::atomic<bool> rendered{false};
            std::thread stop([&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (presentation.running() && std::chrono::steady_clock::now() < deadline) {
                    if (shown()) {
                        rendered = true;
                        if (cycle % 2)
                            presentation.close();
                        else
                            presentation.send(ct::ui::Operation::Quit);
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                presentation.close();
            });
            std::exception_ptr failure;
            try {
                renderer.run();
            } catch (...) {
                failure = std::current_exception();
            }
            presentation.close();
            stop.join();
            if (failure)
                std::rethrow_exception(failure);
            assert(rendered && submitted() >= 3 && !shutdowns());
            assert(runtime.system && runtime.overlay);
        }
        assert(initialized() == 1 && shutdowns() == 1 && created() == destroyed());
        if (!cycle)
            before = descriptors();
        else
            assert(descriptors() == before);
    }
    reset();
    {
        ct::Runtime runtime(true, library);
        ct::ui::Presentation presentation;
        assert(!setenv("CT_TEST_OPENVR_OVERLAY_FAILURE", "1", 1));
        bool rejected = false;
        try {
            ct::ui::Renderer renderer(runtime, presentation, root);
        } catch (const std::exception &) {
            rejected = true;
        }
        unsetenv("CT_TEST_OPENVR_OVERLAY_FAILURE");
        assert(rejected && created() == destroyed() && !shutdowns());
    }
    unsetenv("CT_TEST_OPENVR_HEAD_VALID");
    assert(shutdowns() == 1 && !dlclose(module));
    ButtonGlyphs::instance().cache.clear();
    cairo_debug_reset_static_data();
    FcFini();
    std::cout << "UI renderer: five real GPU lifecycles, one shared OpenVR runtime, model and "
                 "dashboard submission, bounded exit and partial startup cleanup passed\n";
}
