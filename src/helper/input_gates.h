#pragma once
#include "backend.h"
#include "gate.h"
#include "recovery.h"
#include <memory>

namespace ct::helper {
class InputGates final : public RecoveryActions {
    class Owner;
    TrackingBackend &backend_;
    RecoveryOutput output_;
    std::unique_ptr<Owner> input_, beam_;
    std::string input_error_, beam_error_;
    void error(const char *operation, const std::string &text);
    void state(const char *operation, uint32_t index);
    void close(std::unique_ptr<Owner> &owner, const char *operation, bool force = false);

  public:
    explicit InputGates(TrackingBackend &backend);
    ~InputGates() override;
    void start(const RecoveryOutput &output) override;
    void command(uint8_t value, double now) override;
    void poll(double now) override;
    void release_input() override;
    void restore_tracking() override;
};
} // namespace ct::helper
