#include "fake_controller.h"
#include "controller/device.h"
#include "platform/linux/error.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <system_error>

namespace {
using ct::test::device;
using ct::test::FakeState;
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
    {
        const auto state = std::make_shared<FakeState>();
        auto controller = device(state);
        for (unsigned cycle = 0; cycle < 100; ++cycle) {
            controller.enable_imu();
            controller.enable_ir(100);
            assert(state->raw && state->brightness == 100);
            controller.restore();
            assert(!state->closed && !state->raw && state->brightness == 25);
            assert(!controller.changed_imu && !controller.old_brightness);
            assert(controller.read(.01));
            const auto commands = state->commands.size();
            controller.restore();
            assert(state->commands.size() == commands);
        }
        controller.close();
        assert(state->closed == 1);
    }
    {
        const auto state = std::make_shared<FakeState>();
        state->feature_misses = 2;
        auto controller = device(state);
        assert(controller.serial() == "TEST1234" && state->feature_reads == 3);
        const auto serial_commands = state->commands.size();
        for (unsigned tick = 0; tick < 1000; ++tick)
            assert(controller.serial() == "TEST1234");
        assert(state->commands.size() == serial_commands && state->feature_reads == 3);
        controller.enable_imu();
        assert(controller.changed_imu && state->raw);
        controller.enable_ir(100);
        controller.enable_ir(60);
        assert(controller.old_brightness == 25 && state->brightness == 60);
        const auto count = state->commands.size();
        controller.close();
        assert(state->commands.size() == count + 2);
        assert(state->commands[count] == ct::setting_report(0x24, 25));
        assert(state->commands[count + 1] == ct::setting_report(0x30, 0));
        assert(controller.cleanup.imu_disabled_behavior_verified == true);
        assert(controller.cleanup.ir_returned_to_read_value == 25);
        controller.close();
        assert(state->closed == 1 && state->commands.size() == count + 2);
        rejects([&] { controller.read(0); });
        rejects([&] { controller.serial(); });
    }
    {
        const auto state = std::make_shared<FakeState>();
        state->raw = true;
        state->brightness = 0;
        auto controller = device(state);
        controller.enable_imu();
        assert(!controller.changed_imu);
        controller.enable_ir(100);
        controller.close();
        assert(state->raw && state->brightness == 0 &&
               controller.cleanup.ir_returned_to_read_value == 0);
        assert(!controller.cleanup.imu_disabled_behavior_verified);
    }
    for (int code : {ENODEV, EIO, ENXIO, EPERM}) {
        const auto state = std::make_shared<FakeState>();
        auto controller = device(state);
        controller.enable_ir();
        state->restoration_error = code;
        if (code == EPERM)
            rejects([&] { controller.close(); });
        else
            controller.close();
        assert(state->closed == 1);
        assert(controller.cleanup.device_disconnected == (code != EPERM));
        controller.close();
        assert(state->closed == 1);
    }
    {
        const auto state = std::make_shared<FakeState>();
        auto controller = device(state);
        controller.enable_imu();
        state->ignore_disable = true;
        rejects([&] { controller.close(); });
        assert(controller.cleanup.imu_disabled_behavior_verified == false && state->closed == 1);
    }
    {
        const auto state = std::make_shared<FakeState>();
        state->feature_misses = 20;
        auto controller = device(state);
        bool timed_out = false;
        try {
            controller.serial();
        } catch (const ct::TimeoutError &error) {
            timed_out = std::string(error.what()) == "feature query 0xae did not respond";
        }
        assert(timed_out && state->feature_reads == 20);
        assert(std::abs(state->time - 1.4) < 1e-10);
        state->feature_misses = 0;
        assert(controller.serial() == "TEST1234" && state->feature_reads == 21);
        assert(controller.serial() == "TEST1234" && state->feature_reads == 21);
    }
    {
        const auto state = std::make_shared<FakeState>();
        state->reports = false;
        auto controller = device(state);
        rejects([&] { controller.enable_imu(); });
        assert(!controller.changed_imu);
    }
    {
        const auto root = std::filesystem::temp_directory_path() /
                          ("controller-utils-hid-" + std::to_string(getpid()));
        if (!std::filesystem::create_directory(root))
            throw std::runtime_error("test directory already exists");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::filesystem::remove_all(path);
            }
        } cleanup{root};
        auto write = [&](const char *node, const char *text) {
            const auto path = root / node / "device";
            std::filesystem::create_directories(path);
            std::ofstream(path / "uevent") << text;
        };
        rejects([&] {
            ct::LinuxHidTransport::discover(ct::ControllerConnection::Bluetooth, root, "/test-dev");
        });
        write("hidraw0", "HID_ID=0003:000028DE:00001304\nHID_PHYS=usb/input1\n");
        write("hidraw1", "HID_ID=0003:000028DE:00001304\nHID_PHYS=usb/input2\n");
        write("hidraw2", "HID_ID=0005:000028DE:00001303\n");
        write("hidraw3", "HID_ID\n");
        assert(ct::LinuxHidTransport::discover(ct::ControllerConnection::Puck, root, "/test-dev") ==
               "/test-dev/hidraw1");
        assert(ct::LinuxHidTransport::discover(ct::ControllerConnection::Bluetooth, root,
                                               "/test-dev") == "/test-dev/hidraw2");
        write("hidraw4", "HID_ID=0005:000028DE:00001303\n");
        rejects([&] {
            ct::LinuxHidTransport::discover(ct::ControllerConnection::Bluetooth, root, "/test-dev");
        });
    }
    std::cout << "controller device: report ownership, feature replies, restoration and discovery "
                 "passed\n";
}
