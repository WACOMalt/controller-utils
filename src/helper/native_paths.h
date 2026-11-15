#pragma once

namespace ct::helper {
inline constexpr char TrackerDirectory[] = "/var/lib/controllertracking/native-tracker";
inline constexpr char TrackerLibrary[] =
    "/var/lib/controllertracking/native-tracker/libnative_tracker.so";
inline constexpr char TrackerStatusDirectory[] = "/run/controllertracking-native-tracker";
inline constexpr char TrackingLock[] = "/run/controllertracking-native-tracking.lock";
} // namespace ct::helper
