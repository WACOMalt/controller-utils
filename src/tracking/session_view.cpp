#include "session_view.h"

namespace ct {
json::Value tracking_feedback(double now, const std::string &state, const std::string &text,
                              std::optional<double> progress) {
    json::Value result(json_object_new_object());
    json_object_object_add(result.get(), "op", json_object_new_string("tracking_feedback"));
    json_object_object_add(result.get(), "active", json_object_new_boolean(true));
    json_object_object_add(result.get(), "time", json_object_new_double(now));
    json_object_object_add(result.get(), "state", json_object_new_string(state.c_str()));
    json_object_object_add(result.get(), "message", json_object_new_string(text.c_str()));
    json_object_object_add(result.get(), "progress", json::value(progress));
    return result;
}
json::Value alignment_hold_feedback(const AlignmentGuide &guide, double now) {
    const auto hold = guide.progress(now);
    if (!hold)
        return {};
    if (guide.hold_fired)
        return tracking_feedback(now, "restart",
                                 "Release View + Menu for heading correction, or keep holding "
                                 "to 5 seconds to restart tracking.",
                                 hold);
    return tracking_feedback(
        now, "realign", "Point the gamepad where you are looking. Keep holding View + Menu.", hold);
}
} // namespace ct
