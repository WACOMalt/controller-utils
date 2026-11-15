#pragma once
#include "platform/linux/files.h"
#include "platform/linux/process.h"
#include <cerrno>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <time.h>

namespace ct {
class StartupRecovery {
    std::filesystem::path file_;
    std::string boot_;
    mutable uint64_t nonce_ = 0;
    struct State {
        std::string boot, phase;
        unsigned failures = 0, pid = 0;
        uint64_t started = 0, nonce = 0;
    };
    State read() const {
        if (!std::filesystem::exists(file_))
            return {boot_, "clean", 0, 0, 0, 0};
        std::istringstream in(read_file(file_, 256));
        State value;
        std::string extra;
        if (!(in >> value.boot >> value.phase >> value.failures >> value.pid >> value.started >>
              value.nonce) ||
            (in >> extra) ||
            (value.phase != "clean" && value.phase != "starting" && value.phase != "healthy" &&
             value.phase != "disabled" && value.phase != "retry" && value.phase != "error") ||
            value.failures > 2 || value.boot.size() != 36 ||
            (value.phase == "starting" && (!value.pid || !value.started || !value.nonce)))
            throw std::runtime_error("Invalid tracking recovery state");
        return value;
    }
    void write(const State &value) const {
        atomic_write(file_, value.boot + " " + value.phase + " " + std::to_string(value.failures) +
                                " " + std::to_string(value.pid) + " " +
                                std::to_string(value.started) + " " + std::to_string(value.nonce) +
                                "\n");
    }
    static ProcessStamp process(unsigned pid) {
        return process_stamp(read_file("/proc/" + std::to_string(pid) + "/stat", 16384));
    }
    static bool alive(const State &state) {
        try {
            const auto stamp = process(state.pid);
            return stamp.started == state.started && stamp.state != 'Z' && stamp.state != 'X';
        } catch (const std::system_error &error) {
            if (error.code().value() == ENOENT || error.code().value() == ESRCH)
                return false;
            throw;
        }
    }

  public:
    StartupRecovery(std::filesystem::path file, std::string boot)
        : file_(std::move(file)), boot_(std::move(boot)) {
        if (boot_.size() != 36)
            throw std::invalid_argument("Invalid boot identity");
    }
    bool disabled() const {
        const auto phase = read().phase;
        return phase == "disabled" || phase == "retry" || phase == "error";
    }
    bool failed() const {
        return read().phase == "error";
    }
    bool begin() const {
        auto state = read();
        if (state.phase == "disabled" || state.phase == "error")
            return false;
        if (state.phase == "retry" || state.boot != boot_)
            state.failures = 0;
        else if (state.phase == "starting") {
            if (alive(state))
                throw std::runtime_error("Tracking startup is already running");
            ++state.failures;
        }
        state.boot = boot_;
        state.phase = state.failures >= 2 ? "disabled" : "starting";
        timespec timestamp{};
        if (clock_gettime(CLOCK_MONOTONIC, &timestamp))
            throw std::system_error(errno, std::generic_category(), "Read tracking startup clock");
        nonce_ = uint64_t(timestamp.tv_sec) * 1000000000ull + uint64_t(timestamp.tv_nsec);
        state.nonce = state.phase == "starting" ? nonce_ : 0;
        state.pid = state.phase == "starting" ? unsigned(getpid()) : 0;
        state.started = state.pid ? process(state.pid).started : 0;
        write(state);
        return state.phase != "disabled";
    }
    void healthy() const {
        auto state = read();
        if (state.phase == "starting" && state.boot == boot_ && state.pid == unsigned(getpid()) &&
            state.started == process(state.pid).started && state.nonce == nonce_)
            write({boot_, "healthy", 0, 0, 0, 0});
    }
    void clean() const {
        auto state = read();
        if (state.phase == "starting" && state.boot == boot_ && state.pid == unsigned(getpid()) &&
            state.started == process(state.pid).started && state.nonce == nonce_)
            write({boot_, "clean", 0, 0, 0, 0});
    }
    void retry() const {
        write({boot_, "retry", 0, 0, 0, 0});
    }
    void fail() const {
        write({boot_, "error", 2, 0, 0, 0});
    }
    void disable() const {
        write({boot_, "disabled", 2, 0, 0, 0});
    }
};
inline StartupRecovery startup_recovery(const std::filesystem::path &data) {
    auto boot = read_file("/proc/sys/kernel/random/boot_id", 64);
    if (!boot.empty() && boot.back() == '\n')
        boot.pop_back();
    return StartupRecovery(data / "startup-recovery", boot);
}
} // namespace ct
