#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace ct {
struct AcquisitionQuality {
    struct Slot {
        int index = -1;
        bool connected = false;
        unsigned led_count = 0;
        int seed_status = -1, seed_side = -1;
        std::optional<double> seed_time;
    };
    struct Palm {
        int side = -1;
        bool accepted = false;
        std::vector<double> covariance;
    };
    struct Camera {
        unsigned count = 0, finite_count = 0;
        std::optional<double> rms;
    };
    struct Stereo {
        int slot = -1;
        std::vector<Camera> cameras;
    };
    std::optional<double> time;
    std::vector<Slot> slots;
    std::vector<Palm> palms;
    std::vector<Stereo> groups;
    bool ready() const;
};
enum class AlignmentAction { None, Heading, Restart };

class AlignmentGuide {
  public:
    std::optional<double> started, completed, hold_started, last_input;
    bool hold_fired = false, restart_fired = false;
    void begin(double now);
    void finish(double now);
    AlignmentAction update(uint32_t buttons, std::optional<double> received, double now);
    std::optional<double> progress(double now) const;
};

class InitialAcquisition {
  public:
    std::optional<double> since, latest;
    bool ready = false;
    bool update(double timestamp, bool good);
};
} // namespace ct
