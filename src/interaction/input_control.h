#pragma once
#include "controller/report.h"
#include "steamvr/codec.h"
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ct {
struct FilterRequest {
    bool enabled = false;
    bool details = false, typing = false;
    uint8_t touches = 0;
    bool held = false;
    bool system_only = false;
    uint32_t tap = 0;
};
struct FilterState {
    bool active;
    int index;
    std::string serial;
    double time;
    std::string keyboard_health = "off";
    std::string keyboard_overlay = {};
};
struct FilterError {
    std::string message;
};
using FilterEvent = std::variant<FilterState, FilterError>;

class InputBackend {
  public:
    virtual ~InputBackend() = default;
    virtual bool has_filter() const = 0;
    virtual bool filter_running() const = 0;
    virtual void send_filter(const FilterRequest &request) = 0;
    virtual std::vector<FilterEvent> poll_filter() = 0;
    virtual void helper_command(uint8_t command) = 0;
    virtual bool release_controls() = 0;
    virtual std::optional<unsigned> pointer_device(const std::string &serial) = 0;
};

class InputControl {
    InputBackend &backend_;
    std::string serial_;
    std::function<double()> monotonic_, raw_;
    void request_filter(bool enabled, double now, bool typing = false);
    void disable();

  public:
    struct State {
        std::optional<InputControls> input;
        std::optional<double> received, raw_received;
        uint32_t buttons = 0;
        bool enabled = false, typing = false;
        bool gate_ready = false, native_active = false;
        bool beam_ready = false, beam_hidden = false;
        std::optional<unsigned> beam_device;
        double last_beam_heartbeat = 0;
        std::optional<std::string> error;
        std::string keyboard_health = "off";
        bool filter_requested = false, filter_active = false, filter_typing = false;
        std::optional<int> filter_index, gate_index;
        std::optional<double> filter_confirmed, filter_epoch, gate_confirmed, gate_epoch;
        std::optional<int> gate_confirmed_index;
        double last_filter_heartbeat = 0, last_gate_heartbeat = 0;
    } state;
    InputControl(InputBackend &backend, std::string serial, std::function<double()> monotonic = {},
                 std::function<double()> raw = {});
    void controls(const std::optional<ControllerInput> &input, bool enabled, bool typing = false);
    std::vector<std::string> release(bool typing = false);
    void fail(const std::string &message);
    bool gate_ack(int index, double time);
    bool poll(double now, bool tracking_ready);
    void update_beam(double now);
};
} // namespace ct
