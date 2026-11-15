#include "reader.h"
#include "platform/linux/error.h"
#include <stdexcept>
#include <system_error>

namespace ct {
ControllerReader::ControllerReader(std::function<std::shared_ptr<ControllerDevice>()> open,
                                   std::function<double()> monotonic)
    : open_(
          open
              ? std::move(open)
              : [] { return std::make_shared<ControllerDevice>(ControllerConnection::Bluetooth); }),
      monotonic_(monotonic ? std::move(monotonic) : monotonic_time) {}

std::vector<ControllerInput> ControllerReader::poll(bool awake) {
    if (!awake) {
        close();
        return {};
    }
    const auto now = monotonic_();
    if (!device_) {
        if (now < retry_)
            return {};
        retry_ = now + 1;
        try {
            auto device = open_();
            if (!device)
                throw std::runtime_error("controller device was not opened");
            device->serial();
            device_ = std::move(device);
        } catch (const std::runtime_error &) {
            return {};
        }
    }
    std::vector<ControllerInput> reports;
    try {
        for (unsigned i = 0; i < 128; ++i) {
            auto sample = device_->read(0);
            if (!sample)
                break;
            reports.push_back(*sample);
        }
    } catch (const std::system_error &) {
        close();
        reports.clear();
    } catch (const ConnectionError &) {
        close();
        reports.clear();
    }
    return reports;
}
} // namespace ct
