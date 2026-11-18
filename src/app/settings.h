#pragma once
#include "platform/json.h"
#include <cstdint>
#include <filesystem>

namespace ct {
struct Settings {
    int brightness = 100, smoothing_ms = 20;
    bool tracking_enabled = true, autostart = true, pinned = true;
    uint32_t pointer_buttons = 0x80;
    bool steam_shortcuts = true, automatic_updates = true;
    static Settings from_json(json_object *value);
    json::Value to_json() const;
};
Settings load_settings(const std::filesystem::path &path);
void save_settings(const std::filesystem::path &path, const Settings &options);
} // namespace ct
