#pragma once
#include "runtime.h"
#include <array>
#include <functional>
#include <optional>
#include <string>

namespace ct {
namespace steamvr {
class Module;
}
struct MailboxMethod {
    size_t size;
    const char *signature;
};
bool validate_mailbox_table(const steamvr::Module &module, const uintptr_t *table,
                            const std::array<MailboxMethod, 3> &methods);
bool validate_progress_table(const steamvr::Module &module, const uintptr_t *table,
                             size_t target_size, const char *target_signature);
enum class ShortcutAction { Recenter, Passthrough, Screenshot };
struct ShortcutProgress {
    ShortcutAction action;
    double amount;
};

class ShortcutBackend {
  public:
    virtual ~ShortcutBackend() = default;
    virtual uint64_t open(const std::string &name) = 0;
    virtual void send(uint64_t handle, const std::string &target, const std::string &message) = 0;
    virtual void close(uint64_t handle) = 0;
    virtual bool roomview_enabled() = 0;
    virtual void prepare_progress() = 0;
    virtual void post_progress(const ShortcutProgress &event) = 0;
    virtual void post_event(const vr::VREvent_t &event) = 0;
};

class OpenVrShortcutBackend final : public ShortcutBackend {
    Runtime &runtime_;
    void *mailbox_ = nullptr;
    void **mailbox_table_ = nullptr;
    void *client_ = nullptr;
    void (*post_)(void *, const vr::VREvent_t *, uint32_t) = nullptr;
    std::array<uint64_t, 4> progress_paths_{};
    void prepare_events();

  public:
    explicit OpenVrShortcutBackend(Runtime &runtime);
    uint64_t open(const std::string &name) override;
    void send(uint64_t handle, const std::string &target, const std::string &message) override;
    void close(uint64_t handle) override;
    bool roomview_enabled() override;
    void prepare_progress() override;
    void post_progress(const ShortcutProgress &event) override;
    void post_event(const vr::VREvent_t &event) override;
};

class NativeShortcuts {
    ShortcutBackend &backend_;
    std::function<double()> monotonic_;
    uint64_t handle_ = 0;
    bool progress_ready_ = false;
    std::optional<ShortcutAction> progress_action_;
    double progress_last_ = 0;

  public:
    explicit NativeShortcuts(ShortcutBackend &backend, std::function<double()> monotonic = {},
                             std::string group = {});
    NativeShortcuts(const NativeShortcuts &) = delete;
    NativeShortcuts &operator=(const NativeShortcuts &) = delete;
    ~NativeShortcuts();
    void send_action(ShortcutAction action);
    void progress(std::optional<ShortcutAction> action, double amount = 0);
    void close();
};
bool read_roomview_state(const steamvr::Module &module, uintptr_t chaperone);
} // namespace ct
