#pragma once
#include "interaction/input_control.h"
#include "platform/json.h"

namespace ct::test {
inline json::Value filter_request_json(const FilterRequest &request) {
    const auto &[enabled, details, typing, touches, held, system_only, tap] = request;
    json::Value result(json_object_new_object());
    json_object_object_add(result.get(), "enabled", json_object_new_boolean(enabled));
    if (details) {
        json_object_object_add(result.get(), "typing", json_object_new_boolean(typing));
        json_object_object_add(result.get(), "touches", json_object_new_int(touches));
        json_object_object_add(result.get(), "held", json_object_new_boolean(held));
    }
    if (system_only) {
        json_object_object_add(result.get(), "systemOnly", json_object_new_boolean(true));
        json_object_object_add(result.get(), "tap", json_object_new_uint64(tap));
    }
    return result;
}
inline std::optional<FilterEvent> filter_event(json_object *message) {
    auto op = json::find(message, "op");
    if (!op || !json_object_is_type(op, json_type_string))
        return std::nullopt;
    const auto operation = json::string(op);
    if (operation == "input_filter_error") {
        auto text = json::find(message, "message");
        return FilterError{text ? json::string(text).substr(0, 300) : "Steam input filter failed."};
    }
    if (operation != "input_filter_state")
        return std::nullopt;
    try {
        const auto index = json::integer(json::field(message, "index"));
        if (index < -1 || index > 15)
            throw std::invalid_argument("invalid index");
        FilterState state{json::boolean(json::field(message, "active")), int(index),
                          json::string(json::field(message, "serial")),
                          json::number(json::field(message, "monotonic_time")), "off"};
        if (auto health = json::find(message, "keyboardHealth"))
            state.keyboard_health = json::string(health);
        return state;
    } catch (const std::exception &) {
        throw std::invalid_argument("Invalid Steam input filter confirmation.");
    }
}
} // namespace ct::test
