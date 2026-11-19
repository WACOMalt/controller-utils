#pragma once
#include "math.h"

namespace ct {
using PalmPoints = std::array<std::optional<Vector3>, 2>;

class HandPosition {
    struct Grip {
        std::optional<Vector3> offset, quiet_position, previous;
        Vector3 enrollment_body{};
        double since = 0, agreed = 0;
        bool learned = false;
    };
    std::array<Grip, 2> grips_;
    double sample_ = 0;
    struct Correction {
        double captured, received, duration;
        Vector3 from, to;
    };
    struct GripReference {
        double captured;
        Vector3 center;
        PalmPoints offsets;
    };
    struct DriftLimit {
        Vector3 origin, stable_point;
        double stable_since;
        struct Rest {
            double time;
            Vector3 point;
        };
        std::optional<Rest> last_rest{};
        std::array<std::optional<double>, 2> palm_agreement{};
        std::optional<double> released{};
    };
    std::optional<GripReference> grip_reference_;
    std::optional<DriftLimit> drift_;
    std::optional<Correction> assisted_;
    std::optional<double> agreement_since_;
    double native_since_ = 0, native_resumed_ = 0, last_native_ = 0;
    std::optional<double> return_started_;

  public:
    void observe(double time, double received, const PalmPoints &palms,
                 const Quaternion &orientation, const std::optional<Vector3> &native,
                 const Vector3 &display, bool moving);
    std::optional<Vector3> grip_center(size_t side, double stamp, const Vector3 &palm,
                                       const Quaternion &orientation) const;
    void native_observed(double stamp, double received, const Vector3 &position, bool moving,
                         bool palm_relocated = false);
    Vector3 value(double now, const Vector3 &base) const;
    std::optional<double> recent(double now) const;
};
} // namespace ct
