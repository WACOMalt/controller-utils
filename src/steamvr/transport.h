#pragma once
#include "codec.h"
#include "queue_abi.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ct {
class QueueContext {
    steamvr::BlockQueue &queue_;
    uint64_t writer_ = 0;

  public:
    explicit QueueContext(steamvr::BlockQueue &queue) : queue_(queue) {}
    QueueContext(const QueueContext &) = delete;
    QueueContext &operator=(const QueueContext &) = delete;
    ~QueueContext();
    steamvr::BlockQueue &queue() {
        return queue_;
    }
    uint64_t command_writer();
    void preflight();
    void close();
};

class NativeTracking {
    QueueContext &context_;
    std::function<double()> monotonic_;
    std::function<void(double)> sleep_;
    std::vector<uint64_t> handles_;
    std::string profile_;
    bool closed_ = false;
    uint64_t source_ = 0, status_queue_ = 0, pose_queue_ = 0;
    uint32_t previous_input_count_ = 0, last_input_count_ = 0;
    double started_ = 0, last_status_time_ = 0, last_pose_time_ = 0;
    uint64_t open(const char *path, bool optional = false);
    bool write(const wire::Command &command, bool allow_full = false);
    std::optional<std::vector<uint8_t>> read(uint64_t handle, size_t size);

  public:
    bool requested = false;
    std::optional<double> session_start;
    std::optional<wire::Status> state;
    unsigned rejected_poses = 0, rejected_inputs = 0;
    NativeTracking(QueueContext &context, const std::string &profile,
                   std::function<double()> monotonic = {}, std::function<void(double)> sleep = {});
    NativeTracking(const NativeTracking &) = delete;
    NativeTracking &operator=(const NativeTracking &) = delete;
    ~NativeTracking();
    bool sample(double timestamp, const ControllerReport &imu);
    bool controls(double timestamp, const std::optional<BodyPoseMessage> &pose,
                  const InputControls &controls, bool enabled);
    std::optional<NativePoseInput> poll(double now);
    void close();
};
} // namespace ct
