#pragma once
#include "math.h"
#include <deque>
#include <optional>

namespace ct {
class InertialPosition {
  public:
    struct State {
        Vector3 position, velocity;
    };
    struct Sample {
        double time;
        State state;
    };
    struct Observation {
        double time;
        Vector3 position;
    };
    struct Blend {
        double started;
        Vector3 offset;
        double duration;
    };
    double smoothing;
    std::optional<double> time;
    std::optional<Vector3> position, velocity;
    Vector3 acceleration{}, bias{};
    std::deque<Sample> history;
    std::deque<Observation> observations;
    bool stationary = false;
    std::optional<Blend> blend;
    explicit InertialPosition(double smoothing = .04) : smoothing(smoothing) {}
    void sample(double timestamp, const Vector3 &force, double angular_speed);
    std::optional<State> at(double timestamp) const;
    std::optional<Vector3> value(double now) const;
    bool observe(double timestamp, const Vector3 &point, double received, double confidence = 1);
};
} // namespace ct
