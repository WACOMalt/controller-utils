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
    struct Relocation {
        double first, latest;
        Vector3 point;
        unsigned count;
        struct PalmAgreement {
            double first = 0, latest = 0;
        };
        std::optional<std::array<PalmAgreement, 2>> palms{};
    };
    double smoothing;
    std::optional<double> time;
    std::optional<Vector3> position, velocity;
    Vector3 acceleration{}, bias{}, coast_origin{};
    std::deque<Sample> history;
    std::deque<Observation> observations;
    bool stationary = false;
    std::optional<Vector3> stationary_anchor;
    std::optional<Relocation> relocation;
    std::optional<Blend> blend;
    explicit InertialPosition(double smoothing = .04) : smoothing(smoothing) {}
    void sample(double timestamp, const Vector3 &force, double angular_speed);
    void interrupt();
    std::optional<State> at(double timestamp) const;
    std::optional<Vector3> value(double now) const;
    enum class Fix { Rejected, Accepted, PalmRelocated };
    Fix observe(double timestamp, const Vector3 &point, double received, double confidence = 1);
    void observe_grip(size_t side, double timestamp, const std::optional<Vector3> &center);

  private:
    void anchor(double timestamp, const Vector3 &point);
};
} // namespace ct
