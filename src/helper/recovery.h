#pragma once
#include "platform/json.h"
#include "platform/linux/resource.h"
#include <optional>
#include <string>
#include <sys/types.h>

namespace ct::helper {
class RecoveryOutput {
    FileDescriptor output_;

  public:
    explicit RecoveryOutput(int descriptor = 1);
    void emit(json_object *message) const noexcept;
    void error(const std::string &message) const noexcept;
};
class RecoveryActions {
  public:
    virtual ~RecoveryActions() = default;
    virtual void start(const RecoveryOutput &output) = 0;
    virtual void command(uint8_t value, double now) = 0;
    virtual void poll(double now) = 0;
    virtual void release_input() = 0;
    virtual void restore_tracking() = 0;
};
class RecoveryGuard {
    RecoveryActions &actions_;
    FileDescriptor channel_;
    pid_t child_ = -1;
    std::optional<int> status_;
    bool fallback_attempted_ = false;
    void write(const std::string &bytes);

  public:
    RecoveryGuard(RecoveryActions &actions, int output = 1, double lease = 3);
    RecoveryGuard(const RecoveryGuard &) = delete;
    RecoveryGuard &operator=(const RecoveryGuard &) = delete;
    ~RecoveryGuard();
    pid_t pid() const {
        return child_;
    }
    void heartbeat(const std::string &bytes = ".");
    void finish(bool tracking_restored, double timeout = 12);
};
} // namespace ct::helper
