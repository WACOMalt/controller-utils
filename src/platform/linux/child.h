#pragma once
#include "resource.h"
#include "platform/json.h"
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace ct {
class ChildChannel {
  public:
    virtual ~ChildChannel() = default;
    virtual std::optional<int> exit_status() = 0;
    virtual void send(const std::string &bytes) = 0;
    void send(json_object *message);
    virtual std::vector<json::Value> poll() = 0;
    virtual std::vector<json::Value> close(const std::string &command, double timeout = 12) = 0;
    std::vector<json::Value> close(json_object *command, double timeout = 12);
};
using ChildFactory = std::function<std::unique_ptr<ChildChannel>(const std::vector<std::string> &)>;
class ChildProcess final : public ChildChannel {
    FileDescriptor input_, output_;
    pid_t pid_ = -1;
    std::optional<int> status_;
    std::optional<std::string> failure_;
    bool input_closed_ = false, output_closed_ = false;
    std::string buffer_;
    std::deque<std::pair<json::Value, size_t>> messages_;
    size_t queued_bytes_ = 0;
    void read();
    void close_input() noexcept;

  public:
    static constexpr size_t MessageLimit = 4 * 1024 * 1024;
    ChildProcess(const std::vector<std::string> &command, const std::filesystem::path &directory);
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    ~ChildProcess() override;
    pid_t pid() const {
        return pid_;
    }
    std::optional<int> exit_status() override;
    void send(const std::string &bytes) override;
    using ChildChannel::close;
    using ChildChannel::send;
    std::vector<json::Value> poll() override;
    std::vector<json::Value> close(const std::string &command, double timeout = 12) override;
};
} // namespace ct
