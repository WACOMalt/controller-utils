#include "settings.h"
#include "interaction/gestures.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace ct {
Settings load_settings(const std::filesystem::path &path) {
    if (!std::filesystem::exists(path))
        return {};
    auto saved = json::parse(read_file(path, 131072));
    return Settings::from_json(saved.get());
}
void save_settings(const std::filesystem::path &path, const Settings &options) {
    auto value = options.to_json();
    Settings::from_json(value.get());
    atomic_write(path, json::encode(value.get()));
}
Settings Settings::from_json(json_object *value) {
    if (!value || json_object_get_type(value) != json_type_object)
        throw std::invalid_argument("invalid saved settings");
    constexpr std::array<const char *, 8> names{
        "brightness", "smoothing_ms",    "tracking_enabled", "autostart",
        "pinned",     "pointer_buttons", "steam_shortcuts",  "automatic_updates"};
    json_object_object_foreach(value, key, item) {
        (void)item;
        if (std::none_of(names.begin(), names.end(),
                         [&](const char *name) { return std::string(key) == name; }))
            throw std::invalid_argument("invalid saved settings");
    }
    json_object *required = nullptr;
    if (!json_object_object_get_ex(value, "brightness", &required) ||
        !json_object_object_get_ex(value, "smoothing_ms", &required))
        throw std::invalid_argument("invalid saved settings");
    Settings result;
    auto integer = [&](const char *name, int maximum) {
        auto field = json::find(value, name);
        if (!field || json_object_get_type(field) != json_type_int ||
            json_object_get_int64(field) < 0 || json_object_get_int64(field) > maximum)
            throw std::invalid_argument(std::string("invalid ") + name);
        return int(json_object_get_int64(field));
    };
    result.brightness = integer("brightness", 100);
    result.smoothing_ms = integer("smoothing_ms", 150);
    auto flag = [&](const char *name, bool current) {
        json_object *field = nullptr;
        if (!json_object_object_get_ex(value, name, &field))
            return current;
        if (!field || json_object_get_type(field) != json_type_boolean)
            throw std::invalid_argument(std::string("invalid ") + name);
        return bool(json_object_get_boolean(field));
    };
    result.tracking_enabled = flag("tracking_enabled", result.tracking_enabled);
    result.autostart = flag("autostart", result.autostart);
    result.pinned = flag("pinned", result.pinned);
    result.steam_shortcuts = flag("steam_shortcuts", result.steam_shortcuts);
    json_object *binding = nullptr;
    if (json_object_object_get_ex(value, "pointer_buttons", &binding)) {
        if (!binding || json_object_get_type(binding) != json_type_int ||
            json_object_get_int64(binding) <= 0 ||
            uint64_t(json_object_get_int64(binding)) > ButtonMask ||
            (uint64_t(json_object_get_int64(binding)) & ~uint64_t(ButtonMask)))
            throw std::invalid_argument("invalid pointer binding");
        result.pointer_buttons = uint32_t(json_object_get_int64(binding));
    }
    return result;
}
json::Value Settings::to_json() const {
    json::Value result(json_object_new_object());
    json_object_object_add(result.get(), "brightness", json_object_new_int(brightness));
    json_object_object_add(result.get(), "smoothing_ms", json_object_new_int(smoothing_ms));
    json_object_object_add(result.get(), "tracking_enabled",
                           json_object_new_boolean(tracking_enabled));
    json_object_object_add(result.get(), "autostart", json_object_new_boolean(autostart));
    json_object_object_add(result.get(), "pinned", json_object_new_boolean(pinned));
    json_object_object_add(result.get(), "pointer_buttons", json_object_new_int64(pointer_buttons));
    json_object_object_add(result.get(), "steam_shortcuts",
                           json_object_new_boolean(steam_shortcuts));
    return result;
}
} // namespace ct
