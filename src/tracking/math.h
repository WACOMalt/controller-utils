#pragma once
#include "controller/report.h"
#include <array>
#include <deque>
#include <optional>

namespace ct {
using RigidPose = std::array<double, 12>;
double dot(const Vector3 &a, const Vector3 &b);
double dot(const Quaternion &a, const Quaternion &b);
double norm(const Vector3 &value);
double distance(const Vector3 &a, const Vector3 &b);
Vector3 cross(const Vector3 &a, const Vector3 &b);
Vector3 add(const Vector3 &a, const Vector3 &b);
Vector3 subtract(const Vector3 &a, const Vector3 &b);
Vector3 scale(const Vector3 &value, double factor);
bool finite(const Vector3 &value);
bool finite(const Quaternion &value);
Quaternion multiply(const Quaternion &a, const Quaternion &b);
Quaternion conjugate(const Quaternion &value);
Vector3 rotate(const Quaternion &q, const Vector3 &value);
Quaternion integrate(const Quaternion &q, const Vector3 &angular_velocity, double dt);
void validate_pose(const RigidPose &pose);
RigidPose axes_transform(const Vector3 &x, const Vector3 &z, const Vector3 &position);
Vector3 transform_point(const RigidPose &pose, const Vector3 &point);
RigidPose inverse_pose(const RigidPose &pose);
Quaternion quaternion(const RigidPose &pose);
RigidPose rigid_pose(const Quaternion &q, const Vector3 &position);
Vector3 translation(const RigidPose &pose);

class PoseHistory {
  public:
    struct Sample {
        double time;
        RigidPose pose;
        Quaternion orientation;
    };
    std::deque<Sample> samples;
    void add(double timestamp, const std::optional<RigidPose> &pose);
    std::optional<RigidPose> at(double timestamp) const;
};
} // namespace ct
