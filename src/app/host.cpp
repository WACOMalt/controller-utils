#include "host.h"
#include "tracking/optical_session.h"
#include "platform/linux/files.h"
#include <exception>
#include <iostream>

namespace ct {
NativeApplicationHost::NativeApplicationHost(Runtime &runtime, ControllerReader &reader,
                                             const ApplicationLock &lock,
                                             ui::Presentation &presentation,
                                             std::filesystem::path root, std::filesystem::path data,
                                             ChildFactory children, SteamUiFactory steam_ui)
    : runtime_(runtime), reader_(reader), lock_(lock), presentation_(presentation),
      root_(std::move(root)), data_(std::move(data)),
      version_(json::string(
          json::field(json::parse(read_file(root_ / "release.json", 65536)).get(), "version"))),
      children_(children
                    ? std::move(children)
                    : ChildFactory([directory = root_](const std::vector<std::string> &command) {
                          return std::make_unique<ChildProcess>(command, directory);
                      })),
      steam_ui_(steam_ui ? std::move(steam_ui)
                         : SteamUiFactory([](const std::string &serial, bool keyboard) {
                               return std::make_unique<SteamUiClient>(serial, keyboard);
                           })),
      registration_(
          *static_cast<vr::IVRApplications *>(runtime_.interface(vr::IVRApplications_Version))) {
    if (!lock_.primary())
        throw std::logic_error("Controller Utils requires its exclusive lifetime lock");
    registration_.identify();
}
NativeApplicationHost::~NativeApplicationHost() {
    try {
        close();
    } catch (const std::exception &error) {
        log("Controller Utils host shutdown: " + std::string(error.what()));
    }
}
bool NativeApplicationHost::running() {
    return !closed_ && presentation_.running();
}
void NativeApplicationHost::send(json::Value message) {
    if (closed_)
        throw std::logic_error("Renderer is closed");
    auto command = ui::command(message.get());
    if (!command)
        throw std::invalid_argument("Unsupported Controller Utils UI command");
    presentation_.send(std::move(*command));
}
std::vector<json::Value> NativeApplicationHost::events() {
    if (closed_)
        return {};
    std::vector<json::Value> result;
    for (const auto &event : presentation_.events())
        result.push_back(ui::event_json(event));
    return result;
}
void NativeApplicationHost::save(const Settings &options) {
    save_settings(data_ / "settings.json", options);
}
void NativeApplicationHost::autostart(bool enabled) {
    registration_.autostart(enabled);
}
std::unique_ptr<TrackingSessionView>
NativeApplicationHost::track(const Settings &options, const std::optional<TrackingResume> &resume) {
    auto controller = reader_.device();
    if (!controller)
        throw std::runtime_error("Connect a Steam Controller to start tracking.");
    const auto calibration =
        controller_calibration(data_, runtime_.serial(0), controller->serial());
    return std::make_unique<OpticalSession>(
        runtime_, controller, calibration, options, root_, children_, steam_ui_,
        [data = data_](const auto &value) { save_controller_calibration(data, value); }, resume);
}
NativeShortcuts &NativeApplicationHost::shortcuts() {
    if (!shortcuts_) {
        auto backend = std::make_unique<OpenVrShortcutBackend>(runtime_);
        auto shortcuts = std::make_unique<NativeShortcuts>(*backend);
        shortcut_backend_ = std::move(backend);
        shortcuts_ = std::move(shortcuts);
    }
    return *shortcuts_;
}
void NativeApplicationHost::shortcut(const std::string &action) {
    if (action == "dashboard") {
        if (!system_input_ || system_tap_ == UINT32_MAX)
            throw std::runtime_error("Steam button input is unavailable");
        FilterRequest request;
        request.enabled = request.system_only = true;
        request.tap = ++system_tap_;
        system_input_->send(request);
    } else if (action == "recenter")
        shortcuts().send_action(ShortcutAction::Recenter);
    else if (action == "passthrough")
        shortcuts().send_action(ShortcutAction::Passthrough);
    else if (action == "screenshot")
        shortcuts().send_action(ShortcutAction::Screenshot);
    else if (action == "camera_switch") {
        if (!camera_switch_)
            camera_switch_ =
                children_({(root_ / "controller-utils").string(), "--camera-switch-worker"});
        if (!camera_switch_)
            throw std::runtime_error("Camera shortcut could not start.");
    } else
        throw std::invalid_argument("unsupported Frame shortcut");
}
std::optional<std::string> NativeApplicationHost::camera_switch_error(bool enabled) {
    if (!enabled)
        camera_switch_.reset();
    if (!camera_switch_)
        return {};
    try {
        const auto status = camera_switch_->exit_status();
        if (!status)
            return {};
        auto child = std::move(camera_switch_);
        if (*status == 0)
            return {};
        auto messages = child->poll();
        if (messages.size() == 1)
            return json::string(json::field(messages.front().get(), "error"));
    } catch (const std::exception &error) {
        camera_switch_.reset();
        log(error.what());
    }
    return "Camera shortcut did not complete. Try again.";
}
bool NativeApplicationHost::system_input(bool enabled) {
    const auto controller = reader_.device();
    const auto serial = enabled && controller ? controller->serial() : std::string();
    if (serial != system_serial_ || (system_input_ && !system_input_->running())) {
        auto old = std::move(system_input_);
        system_serial_.clear();
        system_confirmed_.reset();
        system_tap_ = 0;
        if (old)
            old->close();
    }
    if (serial.empty())
        return false;
    if (!system_input_) {
        system_input_ = steam_ui_(serial, false);
        if (!system_input_)
            throw std::runtime_error("Steam button input could not start");
        system_serial_ = serial;
    }
    FilterRequest request;
    request.enabled = request.system_only = true;
    request.tap = system_tap_;
    system_input_->send(request);
    for (const auto &event : system_input_->poll()) {
        if (const auto state = std::get_if<FilterState>(&event)) {
            if (state->active)
                system_confirmed_ = state->time;
            else
                system_confirmed_.reset();
        } else if (const auto error = std::get_if<FilterError>(&event)) {
            system_confirmed_.reset();
            throw std::runtime_error(error->message);
        }
    }
    const auto age = system_confirmed_ ? monotonic_time() - *system_confirmed_ : 1e30;
    return age >= -.02 && age <= .5;
}
void NativeApplicationHost::shortcut_progress(std::optional<double> amount) {
    if (amount)
        shortcuts().progress(ShortcutAction::Recenter, *amount);
    else if (shortcuts_)
        shortcuts_->progress(std::nullopt);
}
void NativeApplicationHost::request_keyboard() {
    if (!keyboard_) {
        keyboard_ = steam_ui_("keyboard-shortcut", true);
        if (!keyboard_)
            throw std::runtime_error("Keyboard shortcut helper could not start");
    }
}
std::optional<std::string> NativeApplicationHost::keyboard_error() {
    if (!keyboard_)
        return std::nullopt;
    if (keyboard_->running())
        return std::nullopt;
    auto request = std::move(keyboard_);
    try {
        request->close();
        for (const auto &event : request->poll()) {
            if (const auto error = std::get_if<FilterError>(&event))
                return error->message;
            const auto &state = std::get<FilterState>(event);
            if (!state.keyboard_overlay.empty()) {
                auto overlay = runtime_.overlay;
                vr::VROverlayHandle_t window = vr::k_ulOverlayHandleInvalid;
                if (!overlay || overlay->FindOverlay(state.keyboard_overlay.c_str(), &window) ||
                    !window)
                    throw std::runtime_error("The focused window is no longer available");
                const auto error = overlay->ShowKeyboardForOverlay(
                    window, vr::k_EGamepadTextInputModeNormal,
                    vr::k_EGamepadTextInputLineModeSingleLine,
                    vr::KeyboardFlag_Minimal | vr::KeyboardFlag_HideDoneKey |
                        vr::KeyboardFlag_ShowArrowKeys,
                    "", 1024, "", 0);
                if (error)
                    throw std::runtime_error("Could not open the window keyboard: " +
                                             std::to_string(error));
                overlay->SetKeyboardPositionForOverlay(window, {});
            }
        }
    } catch (const std::exception &error) {
        return error.what();
    }
    return std::nullopt;
}
json::Value NativeApplicationHost::app_info() {
    auto value = json::parse(R"({"op":"app_info"})");
    json_object_object_add(value.get(), "version", json_object_new_string(version_.c_str()));
    return value;
}
void NativeApplicationHost::log(const std::string &message) {
    std::cerr << message << '\n';
}
void NativeApplicationHost::close_keyboard() {
    auto request = std::move(keyboard_);
    if (request)
        request->close();
}
void NativeApplicationHost::close() {
    if (closed_)
        return;
    closed_ = true;
    camera_switch_.reset();
    std::exception_ptr failure;
    const auto attempt = [&](const auto &operation) {
        try {
            operation();
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    };
    attempt([&] { close_keyboard(); });
    attempt([&] {
        if (system_input_)
            system_input_->close();
        system_input_.reset();
    });
    presentation_.close();
    auto shortcuts = std::move(shortcuts_);
    attempt([&] {
        if (shortcuts)
            shortcuts->close();
    });
    if (failure)
        std::rethrow_exception(failure);
}
} // namespace ct
