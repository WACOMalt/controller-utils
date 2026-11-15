#pragma once
#include "platform/linux/process.h"
#include <array>
#include <optional>
#include <vector>
#if defined(__aarch64__)
#include <asm/ptrace.h>
#elif defined(__x86_64__)
#include <sys/user.h>
#else
#error Unsupported native register architecture
#endif

namespace ct::helper {
struct Registers {
#if defined(__aarch64__)
    user_pt_regs native{};
#else
    user_regs_struct native{};
#endif
    uintptr_t pc() const;
    void pc(uintptr_t value);
};
class StoppedProcess {
    struct Thread {
        pid_t id;
        uint64_t started = 0;
        bool attached = false, stopped = false, done = false, exiting = false;
        bool member = true, newborn = false, interrupted = false;
        int signal = 0;
    };
    PinnedProcess &process_;
    pid_t owner_;
    double deadline_;
    std::vector<Thread> threads_;
    bool ready_ = false, changed_ = false, external_stop_ = false;
    Thread *find(pid_t id);
    void add(pid_t id, bool automatic = false);
    void collect();
    void request_interrupts();
    void require_thread(pid_t id) const;

  public:
    explicit StoppedProcess(PinnedProcess &process, double timeout = 1.5);
    StoppedProcess(const StoppedProcess &) = delete;
    StoppedProcess &operator=(const StoppedProcess &) = delete;
    ~StoppedProcess();
    void check() const;
    std::vector<pid_t> threads() const;
    Registers registers(pid_t id) const;
    void move_pc(pid_t id, uintptr_t destination);
    std::vector<uint8_t> read(uintptr_t address, size_t size) const;
    void write_code(uintptr_t address, const std::vector<uint8_t> &bytes);
    std::optional<std::vector<uintptr_t>> stack(pid_t id, size_t limit = 64) const;
    void close(double timeout = .3);
};
} // namespace ct::helper
