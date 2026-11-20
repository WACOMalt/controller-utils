#pragma once
#include "application.h"
#include "instance.h"
#include "registration.h"
#include "controller/reader.h"
#include "steamvr/shortcuts.h"
#include "platform/linux/child.h"
#include "steamvr/steam_ui.h"
#include "ui/presentation.h"

namespace ct {
class NativeApplicationHost final : public ApplicationHost {
    Runtime &runtime_;
    ControllerReader &reader_;
    const ApplicationLock &lock_;
    ui::Presentation &presentation_;
    std::filesystem::path root_, data_;
    std::string version_;
    ChildFactory children_;
    SteamUiFactory steam_ui_;
    ApplicationRegistration registration_;
    std::unique_ptr<SteamUiSession> keyboard_;
    std::unique_ptr<SteamUiSession> system_input_;
    std::unique_ptr<ChildChannel> camera_switch_;
    std::string system_serial_;
    uint32_t system_tap_ = 0;
    std::optional<double> system_confirmed_;
    std::unique_ptr<OpenVrShortcutBackend> shortcut_backend_;
    std::unique_ptr<NativeShortcuts> shortcuts_;
    bool closed_ = false;
    NativeShortcuts &shortcuts();

  public:
    NativeApplicationHost(Runtime &runtime, ControllerReader &reader, const ApplicationLock &lock,
                          ui::Presentation &presentation, std::filesystem::path root,
                          std::filesystem::path data, ChildFactory children = {},
                          SteamUiFactory steam_ui = {});
    ~NativeApplicationHost() override;
    bool running();
    void send(json::Value message) override;
    std::vector<json::Value> events() override;
    void save(const Settings &options) override;
    void autostart(bool enabled) override;
    std::unique_ptr<TrackingSessionView> track(const Settings &options) override;
    void shortcut(const std::string &action) override;
    std::optional<std::string> camera_switch_error(bool enabled) override;
    bool system_input(bool enabled) override;
    void shortcut_progress(std::optional<double> amount) override;
    void request_keyboard() override;
    std::optional<std::string> keyboard_error() override;
    json::Value app_info() override;
    void log(const std::string &message) override;
    void close_keyboard();
    void close();
};
} // namespace ct
