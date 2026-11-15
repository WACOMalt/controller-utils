#include "trace.h"
#include "platform/linux/error.h"
#include "platform/linux/files.h"
#include "platform/linux/resource.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstring>
#include <elf.h>
#include <iostream>
#include <libunwind-ptrace.h>
#include <limits>
#include <memory>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

namespace ct::helper {
namespace {
struct Task {
    pid_t id, group, tracer;
    uint64_t started;
    char state;
};
pid_t decimal(const std::string &text) {
    int value = -1;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < 0)
        throw std::invalid_argument("invalid native task identity");
    return value;
}
bool gone(const std::system_error &error) {
    return error.code().value() == ENOENT || error.code().value() == ESRCH;
}
Task task(const std::filesystem::path &path, pid_t id) {
    const auto stamp = process_stamp(read_file(path / "stat", 16384));
    std::istringstream input(read_file(path / "status", 65536));
    std::string line;
    Task result{id, -1, -1, stamp.started, stamp.state};
    while (std::getline(input, line)) {
        if (line.rfind("Tgid:", 0) != 0 && line.rfind("TracerPid:", 0) != 0)
            continue;
        std::istringstream field(line);
        std::string name, value;
        if (!(field >> name >> value))
            throw std::invalid_argument("invalid native task status");
        if (name == "Tgid:")
            result.group = decimal(value);
        else
            result.tracer = decimal(value);
    }
    if (result.group <= 0 || result.tracer < 0)
        throw std::invalid_argument("incomplete native task identity");
    return result;
}
std::vector<Task> tasks(PinnedProcess &process) {
    std::vector<Task> result;
    for (const auto &entry : std::filesystem::directory_iterator(process.proc() / "task")) {
        const auto id = decimal(entry.path().filename().string());
        try {
            auto info = task(entry.path(), id);
            if (info.group != process.pid())
                throw ConnectionError("native task group identity changed");
            result.push_back(info);
        } catch (const std::system_error &error) {
            if (!gone(error))
                throw;
        }
        if (result.size() > 4096)
            throw std::runtime_error("native task count exceeded limit");
    }
    if (result.empty())
        throw ConnectionError("native process has no live threads");
    return result;
}
long trace(enum __ptrace_request request, pid_t id, void *address = nullptr, void *data = nullptr) {
    errno = 0;
    const auto value = ptrace(request, id, address, data);
    if (value == -1 && errno)
        throw std::system_error(errno, std::generic_category(), "native ptrace operation");
    return value;
}
} // namespace
uintptr_t Registers::pc() const {
#if defined(__aarch64__)
    return native.pc;
#else
    return native.rip;
#endif
}
void Registers::pc(uintptr_t value) {
#if defined(__aarch64__)
    native.pc = value;
#else
    native.rip = value;
#endif
}
StoppedProcess::Thread *StoppedProcess::find(pid_t id) {
    const auto found = std::find_if(threads_.begin(), threads_.end(), [&](const auto &thread) {
        return thread.id == id && !thread.done;
    });
    return found == threads_.end() ? nullptr : &*found;
}
void StoppedProcess::add(pid_t id, bool automatic) {
    if (find(id))
        return;
    if (threads_.size() == threads_.capacity())
        throw std::runtime_error("native trace task count exceeded limit");
    Thread record{id};
    record.newborn = automatic;
    record.attached = automatic;
    record.member = !automatic;
    threads_.push_back(record);
    auto &entry = threads_.back();
    try {
        const auto info = task("/proc/" + std::to_string(id), id);
        entry.started = info.started;
        entry.member = info.group == process_.pid();
        if (automatic) {
            if (info.tracer != owner_)
                throw ConnectionError("new native thread has an unexpected tracer");
            return;
        }
        if (info.tracer == owner_) {
            entry.attached = entry.newborn = true;
            return;
        }
        if (info.tracer)
            throw std::runtime_error("native process is already attached to a debugger");
        if (info.state == 'T' || info.state == 't')
            throw std::runtime_error("native process is already stopped");
        const auto options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT;
        try {
            trace(PTRACE_SEIZE, id, nullptr, reinterpret_cast<void *>(uintptr_t(options)));
            entry.attached = true;
        } catch (const std::system_error &error) {
            if (error.code().value() != EPERM)
                throw;
            const auto current = task("/proc/" + std::to_string(id), id);
            if (current.tracer != owner_)
                throw;
            entry.attached = entry.newborn = true;
        }
    } catch (const std::system_error &error) {
        if (!gone(error))
            throw;
        if (!entry.attached)
            entry.done = true;
    }
}
void StoppedProcess::request_interrupts() {
    for (auto &thread : threads_) {
        if (!thread.attached || thread.done || thread.stopped || thread.interrupted)
            continue;
        try {
            trace(PTRACE_INTERRUPT, thread.id);
            thread.interrupted = true;
        } catch (const std::system_error &error) {
            if (error.code().value() != ESRCH)
                throw;
        }
    }
}
void StoppedProcess::collect() {
    for (size_t i = 0; i < threads_.size(); ++i) {
        auto &thread = threads_[i];
        if (!thread.attached || thread.done || thread.stopped)
            continue;
        int status = 0;
        const auto result = waitpid(thread.id, &status, __WALL | WNOHANG);
        if (result < 0) {
            const int wait_error = errno;
            if (wait_error == EINTR)
                continue;
            if (wait_error == ECHILD || wait_error == ESRCH) {
                try {
                    const auto current = task("/proc/" + std::to_string(thread.id), thread.id);
                    if (current.tracer != owner_) {
                        thread.done = true;
                        continue;
                    }
                } catch (const std::system_error &error) {
                    if (gone(error)) {
                        thread.done = true;
                        continue;
                    }
                    throw;
                }
            }
            throw std::system_error(wait_error, std::generic_category(),
                                    "wait for native task stop");
        }
        if (!result)
            continue;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            thread.done = true;
            continue;
        }
        if (!WIFSTOPPED(status))
            throw std::runtime_error("unexpected native task wait status");
        thread.stopped = true;
        const unsigned event = unsigned(status) >> 16;
        const int signal = WSTOPSIG(status);
        if (event == PTRACE_EVENT_CLONE) {
            unsigned long child = 0;
            trace(PTRACE_GETEVENTMSG, thread.id, nullptr, &child);
            if (!child || child > INT_MAX)
                throw std::runtime_error("invalid cloned native task identity");
            add(pid_t(child), true);
        } else if (event == PTRACE_EVENT_EXEC)
            changed_ = true;
        else if (event == PTRACE_EVENT_EXIT)
            thread.exiting = true;
        else if (event == PTRACE_EVENT_STOP) {
            if (signal != SIGTRAP && !thread.newborn) {
                thread.signal = signal;
                external_stop_ = true;
            }
        } else if (!event)
            thread.signal = signal;
        else
            throw std::runtime_error("unexpected native tracing event");
        thread.newborn = false;
        try {
            const auto info = task("/proc/" + std::to_string(thread.id), thread.id);
            if (info.tracer != owner_ || (thread.started && info.started != thread.started))
                throw ConnectionError("stopped native thread identity changed");
            thread.started = info.started;
            thread.member = info.group == process_.pid();
        } catch (const std::system_error &error) {
            if (!gone(error))
                throw;
        }
    }
}
StoppedProcess::StoppedProcess(PinnedProcess &process, double timeout)
    : process_(process), owner_(pid_t(syscall(SYS_gettid))), deadline_(monotonic_time() + timeout) {
    if (!std::isfinite(timeout) || timeout <= 0 || timeout > 1.5)
        throw std::invalid_argument("invalid native pause deadline");
    threads_.reserve(4096);
    process_.refresh();
    const auto before = tasks(process_);
    for (const auto &thread : before) {
        if (thread.tracer)
            throw std::runtime_error("native process is already attached to a debugger");
        if (thread.state == 'T' || thread.state == 't')
            throw std::runtime_error("native process is already stopped");
    }
    try {
        for (const auto &thread : before) {
            check();
            add(thread.id);
        }
        for (;;) {
            check();
            request_interrupts();
            collect();
            if (changed_)
                throw ConnectionError("native executable changed while stopping");
            if (external_stop_)
                throw std::runtime_error("native process was stopped externally");
            bool complete = true;
            for (const auto &thread : tasks(process_)) {
                auto owned = find(thread.id);
                if (!owned) {
                    add(thread.id);
                    complete = false;
                } else if (!owned->stopped)
                    complete = false;
            }
            complete =
                complete && std::all_of(threads_.begin(), threads_.end(), [](const auto &thread) {
                    return thread.done || (thread.attached && thread.stopped);
                });
            if (complete) {
                process_.refresh();
                ready_ = true;
                return;
            }
            sleep_for(.001);
        }
    } catch (...) {
        const auto failure = std::current_exception();
        try {
            close();
        } catch (const std::exception &error) {
            std::cerr << "native trace detach: " << error.what() << '\n';
            _exit(125);
        }
        std::rethrow_exception(failure);
    }
}
StoppedProcess::~StoppedProcess() {
    try {
        close();
    }
    // Exiting this dedicated worker lets the kernel release any remaining trace ownership.
    catch (const std::exception &error) {
        std::cerr << "native trace detach: " << error.what() << '\n';
        _exit(125);
    }
}
void StoppedProcess::check() const {
    if (pid_t(syscall(SYS_gettid)) != owner_)
        throw std::logic_error("native trace owner thread changed");
    if (!process_.alive())
        throw ConnectionError("native process exited while paused");
    if (monotonic_time() >= deadline_)
        throw TimeoutError("native patch exceeded its bounded pause");
}
void StoppedProcess::require_thread(pid_t id) const {
    check();
    const auto found = std::find_if(threads_.begin(), threads_.end(), [&](const auto &thread) {
        return thread.id == id && thread.member && thread.attached && thread.stopped &&
               !thread.done && !thread.exiting;
    });
    if (!ready_ || found == threads_.end())
        throw std::logic_error("native thread is not owned and stopped");
}
std::vector<pid_t> StoppedProcess::threads() const {
    check();
    if (!ready_)
        throw std::logic_error("native process is not fully stopped");
    std::vector<pid_t> result;
    for (const auto &thread : threads_)
        if (thread.member && thread.attached && thread.stopped && !thread.done && !thread.exiting)
            result.push_back(thread.id);
    return result;
}
Registers StoppedProcess::registers(pid_t id) const {
    require_thread(id);
    Registers result;
    iovec data{&result.native, sizeof(result.native)};
    trace(PTRACE_GETREGSET, id, reinterpret_cast<void *>(NT_PRSTATUS), &data);
    if (data.iov_len != sizeof(result.native))
        throw std::runtime_error("incompatible native register set");
    return result;
}
void StoppedProcess::move_pc(pid_t id, uintptr_t destination) {
    auto before = registers(id);
#if defined(__aarch64__)
    if (destination % 4)
        throw std::invalid_argument("unaligned native program counter");
#endif
    if (!std::any_of(
            process_.mappings().begin(), process_.mappings().end(), [&](const auto &mapping) {
                return mapping.readable && mapping.executable && mapping.contains(destination, 4);
            }))
        throw std::invalid_argument("native program counter is outside executable mappings");
    before.pc(destination);
    iovec data{&before.native, sizeof(before.native)};
    trace(PTRACE_SETREGSET, id, reinterpret_cast<void *>(NT_PRSTATUS), &data);
    const auto after = registers(id);
    if (std::memcmp(&before.native, &after.native, sizeof(before.native)))
        throw std::runtime_error("native register update did not read back");
}
std::vector<uint8_t> StoppedProcess::read(uintptr_t address, size_t size) const {
    check();
    if (!ready_)
        throw std::logic_error("native process is not fully stopped");
    return process_.read(address, size);
}
void StoppedProcess::write_code(uintptr_t address, const std::vector<uint8_t> &bytes) {
    check();
    if (!ready_ || address % 4 || bytes.empty() || bytes.size() % 4 || bytes.size() > 4096 ||
        !std::any_of(process_.mappings().begin(), process_.mappings().end(),
                     [&](const auto &mapping) {
                         return mapping.readable && mapping.executable && !mapping.writable &&
                                mapping.private_mapping && mapping.contains(address, bytes.size());
                     }))
        throw std::invalid_argument("instruction patch is outside the stopped RX mapping");
    const auto ids = threads();
    if (ids.empty())
        throw ConnectionError("native process has no stopped execution context");
    size_t offset = 0;
    while (offset < bytes.size()) {
        check();
        const uintptr_t cursor = address + offset;
        const uintptr_t aligned = cursor & ~uintptr_t(sizeof(long) - 1);
        long word = trace(PTRACE_PEEKTEXT, ids.front(), reinterpret_cast<void *>(aligned));
        const size_t inside = cursor - aligned,
                     count = std::min(sizeof(word) - inside, bytes.size() - offset);
        std::memcpy(reinterpret_cast<uint8_t *>(&word) + inside, bytes.data() + offset, count);
        trace(PTRACE_POKETEXT, ids.front(), reinterpret_cast<void *>(aligned),
              reinterpret_cast<void *>(uintptr_t(word)));
        if (process_.read(cursor, count) !=
            std::vector<uint8_t>(bytes.begin() + offset, bytes.begin() + offset + count))
            throw std::runtime_error("native instruction write did not read back");
        offset += count;
    }
}
std::optional<std::vector<uintptr_t>> StoppedProcess::stack(pid_t id, size_t limit) const {
    require_thread(id);
    if (!limit || limit > 64)
        throw std::invalid_argument("invalid native stack depth limit");
    std::unique_ptr<void, decltype(&_UPT_destroy)> context(_UPT_create(id), _UPT_destroy);
    std::unique_ptr<unw_addr_space, decltype(&unw_destroy_addr_space)> space(
        unw_create_addr_space(&_UPT_accessors, 0), unw_destroy_addr_space);
    if (!context || !space)
        throw std::bad_alloc();
    unw_cursor_t cursor{};
    if (unw_init_remote(&cursor, space.get(), context.get()) < 0)
        return std::nullopt;
    std::vector<uintptr_t> frames;
    for (size_t i = 0; i < limit; ++i) {
        check();
        unw_word_t pc = 0;
        if (unw_get_reg(&cursor, UNW_REG_IP, &pc) < 0 || !pc)
            return std::nullopt;
        frames.push_back(uintptr_t(pc));
        const auto next = unw_step(&cursor);
        if (!next)
            return frames;
        if (next < 0)
            return std::nullopt;
    }
    return std::nullopt;
}
void StoppedProcess::close(double timeout) {
    if (pid_t(syscall(SYS_gettid)) != owner_)
        throw std::logic_error("native trace owner thread changed");
    if (!std::isfinite(timeout) || timeout < 0 || timeout > .3)
        throw std::invalid_argument("invalid native detach deadline");
    ready_ = false;
    const double end = monotonic_time() + timeout;
    for (;;) {
        request_interrupts();
        collect();
        bool remaining = false;
        for (auto it = threads_.rbegin(); it != threads_.rend(); ++it) {
            auto &thread = *it;
            if (!thread.attached || thread.done)
                continue;
            try {
                const auto current = task("/proc/" + std::to_string(thread.id), thread.id);
                if (current.tracer != owner_) {
                    thread.done = true;
                    continue;
                }
                if (thread.started && current.started != thread.started)
                    throw ConnectionError("native detach task identity changed");
                if (!thread.stopped) {
                    remaining = true;
                    continue;
                }
                trace(PTRACE_DETACH, thread.id, nullptr,
                      reinterpret_cast<void *>(uintptr_t(thread.signal)));
                thread.done = true;
            } catch (const std::system_error &error) {
                if (gone(error))
                    thread.done = true;
                else
                    throw;
            }
        }
        if (!remaining)
            return;
        if (monotonic_time() >= end)
            throw TimeoutError("native trace detach was not confirmed");
        sleep_for(.001);
    }
}
} // namespace ct::helper
