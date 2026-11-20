#pragma once
#include "math.h"

namespace ct {
struct BodyPoseMessage {
    double time, position_time;
    Vector3 position;
    double orientation_time;
    Quaternion orientation;
};
} // namespace ct
