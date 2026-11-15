#pragma once
#include "backend.h"
#include "gate.h"
#include "recovery.h"
#include <memory>

namespace ct::helper {
class InputGates final : public RecoveryActions {
  public:
    class Owner {
      public:
        virtual ~Owner() = default;
        virtual uint32_t index() const = 0;
        virtual void apply() = 0;
        virtual void renew(uint32_t index, double now) = 0;
        virtual void disable() = 0;
        virtual void poll(double now) = 0;
        virtual void restore(bool force) = 0;
    };
    using Factory = std::function<std::unique_ptr<Owner>(GateKind, bool restoring)>;

  private:
    TrackingBackend &backend_;
    Factory factory_;
    RecoveryOutput output_;
    std::unique_ptr<Owner> input_, beam_;
    std::string input_error_, beam_error_;
    std::optional<double> input_retry_{0}, beam_retry_{0};
    bool started_ = false;
    void error(const char *operation, const std::string &text);
    void state(const char *operation, uint32_t index);
    void close(std::unique_ptr<Owner> &owner, const char *operation, bool force = false);
    void acquire(GateKind kind, double now);

  public:
    explicit InputGates(TrackingBackend &backend, Factory factory = {});
    ~InputGates() override;
    void start(const RecoveryOutput &output) override;
    void command(uint8_t value, double now) override;
    void poll(double now) override;
    void release_input() override;
    void restore_tracking() override;
};
} // namespace ct::helper
