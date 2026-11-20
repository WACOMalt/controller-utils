#include "tracking/calibration.h"
#include "platform/linux/files.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {
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
    const auto root =
        std::filesystem::current_path() / "build" / ("calibration-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    auto first = ct::controller_calibration(root, "HEADSET-A", "GAMEPAD-A");
    assert(!first.gyro_calibrated && first.body.controller_serial == "GAMEPAD-A");
    assert(first.emitter == (ct::Vector3{0, -.0004572075, .0087}));
    auto second = ct::controller_calibration(root, "HEADSET-B", "GAMEPAD-B");
    assert(second.body.controller_serial == "GAMEPAD-B" && second.headset_serial == "HEADSET-B");
    first.body.gyro_bias_report_rad_s = {.0001, -.0002, .0003};
    first.gyro_calibrated = true;
    ct::save_controller_calibration(root, first);
    auto moved = ct::controller_calibration(root, "HEADSET-C", "GAMEPAD-A");
    assert(moved.headset_serial == "HEADSET-C" && moved.gyro_calibrated);
    assert(moved.body.gyro_bias_report_rad_s == first.body.gyro_bias_report_rad_s);
    assert(!ct::controller_calibration(root, "HEADSET-C", "GAMEPAD-B").gyro_calibrated);
    unsigned files = 0;
    for (const auto &entry : std::filesystem::directory_iterator(root / "calibrations")) {
        const auto bytes = ct::read_file(entry.path(), 131072);
        assert(bytes.find("HEADSET-") == std::string::npos);
        struct stat info{};
        assert(!stat(entry.path().c_str(), &info) && (info.st_mode & 0777) == 0600 &&
               info.st_uid == getuid());
        ++files;
    }
    assert(files == 2);
    rejects([&] { ct::controller_calibration(root, "HEADSET", ""); });
    rejects([&] { ct::controller_calibration(root, "HEADSET", "bad\nserial"); });
    auto broken = first;
    broken.body.gyro_bias_report_rad_s[0] = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { ct::save_controller_calibration(root, broken); });
    assert(ct::controller_calibration(root, "HEADSET", "GAMEPAD-A").body.gyro_bias_report_rad_s ==
           first.body.gyro_bias_report_rad_s);
    for (const auto &entry : std::filesystem::directory_iterator(root / "calibrations")) {
        const auto data = ct::json::parse(ct::read_file(entry.path(), 131072));
        if (ct::json::string(ct::json::field(ct::json::field(data.get(), "body"),
                                             "controller_serial")) == "GAMEPAD-B") {
            std::filesystem::remove(entry.path());
            std::filesystem::create_symlink(root / "untrusted", entry.path());
            rejects([&] { ct::controller_calibration(root, "HEADSET", "GAMEPAD-B"); });
            break;
        }
    }
    ct::ControllerReport report{};
    report.acceleration = {0, 0, 9.80665};
    report.angular_velocity = {.0001, -.0002, .0003};
    std::cout << "Controller calibration: fresh devices, independent storage, headset changes, "
                 "safe paths and stationary IMU measurement passed\n";
}
