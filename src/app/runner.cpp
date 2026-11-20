#include "runner.h"
#include <exception>

namespace ct {
int run_application(NativeApplicationHost &host, Runtime &runtime, ControllerReader &reader,
                    SignalQueue &signals, const std::filesystem::path &data, bool background) {
    Application application(host, load_settings(data / "settings.json"), background);
    std::exception_ptr failure;
    try {
        while (application.running() && host.running()) {
            while (const auto signal = signals.receive()) {
                if (*signal == SIGUSR1)
                    application.show_settings();
                else
                    application.stop();
            }
            if (!application.running())
                break;
            const auto awake = bool(runtime.head_pose());
            const auto reports = reader.poll(awake);
            application.step(awake, reader.connected(), reports);
            sleep_for(application.cadence());
        }
    } catch (...) {
        failure = std::current_exception();
    }
    const auto attempt = [&](const auto &operation) {
        try {
            operation();
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    };
    attempt([&] { host.close_keyboard(); });
    attempt([&] { application.close(); });
    attempt([&] { host.close(); });
    reader.close();
    if (failure)
        std::rethrow_exception(failure);
    return 0;
}
} // namespace ct
