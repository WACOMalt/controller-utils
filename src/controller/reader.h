#pragma once
#include "device.h"
#include <functional>
#include <memory>
#include <vector>

namespace ct {
class ControllerReader {
    std::shared_ptr<ControllerDevice> device_;
    std::function<std::shared_ptr<ControllerDevice>()> open_;
    std::function<double()> monotonic_;
    double retry_ = 0;

  public:
    explicit ControllerReader(std::function<std::shared_ptr<ControllerDevice>()> open = {},
                              std::function<double()> monotonic = {});
    std::vector<ControllerInput> poll(bool awake);
    std::shared_ptr<ControllerDevice> device() const {
        return device_;
    }
    bool connected() const {
        return bool(device_);
    }
    void close() noexcept {
        device_.reset();
    }
};
} // namespace ct
