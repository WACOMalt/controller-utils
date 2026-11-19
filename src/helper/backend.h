#pragma once
#include "acquisition_reader.h"
#include "bootstrap.h"
#include "capability.h"
#include "package.h"
#include "tracker_control.h"
#include "xr_process.h"

namespace ct::helper {
class TrackingBackend {
  public:
    virtual ~TrackingBackend() = default;
    virtual int descriptor() const = 0;
    virtual bool start() = 0;
    virtual void renew() = 0;
    virtual std::vector<json::Value> poll(bool quality_requested) = 0;
    virtual void restore(const std::function<void()> &pulse = {}) = 0;
};
class XrBackend final : public TrackingBackend {
    BackendHost &host_;
    std::function<double()> monotonic_;
    std::function<uint64_t()> raw_;
    std::function<void(double)> sleep_;
    TrackerControl control_;
    AcquisitionReader acquisition_;
    bool started_ = false, announced_ = false;
    unsigned attempts_ = 0;
    double started_at_ = 0, sample_at_ = 0, palms_at_ = 0, frames_at_ = 0;
    uint64_t frames_ = 0;

  public:
    XrBackend(BackendHost &host, std::function<double()> monotonic = {},
              std::function<uint64_t()> raw = {}, std::function<void(double)> sleep = {});
    int descriptor() const override {
        return host_.descriptor();
    }
    bool start() override;
    void renew() override;
    std::vector<json::Value> poll(bool quality_requested) override;
    void restore(const std::function<void()> &pulse = {}) override;
};
} // namespace ct::helper
