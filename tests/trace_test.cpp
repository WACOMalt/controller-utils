#include "helper/trace.h"
#include "helper/worker.h"
#include "helper/gate.h"
#include "platform/linux/child.h"
#include "process_image.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace {
std::atomic<uint64_t> counter{0};
volatile sig_atomic_t signals = 0;
void signal_received(int) {
    signals = 1;
}
std::vector<uint8_t> function(unsigned value) {
#if defined(__x86_64__)
    return {0xb8, uint8_t(value), 0, 0, 0, 0xc3, 0x90, 0x90};
#elif defined(__aarch64__)
    const uint32_t words[]{0x52800000u | (value << 5), 0xd65f03c0};
    std::vector<uint8_t> bytes(sizeof(words));
    std::memcpy(bytes.data(), words, sizeof(words));
    return bytes;
#endif
}
int target() {
    assert(!prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY));
    struct sigaction action{};
    action.sa_handler = signal_received;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1, &action, nullptr));
    void *page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    const auto bytes = function(7);
    std::memcpy(page, bytes.data(), bytes.size());
    __builtin___clear_cache(static_cast<char *>(page), static_cast<char *>(page) + bytes.size());
    assert(!mprotect(page, 4096, PROT_READ | PROT_EXEC));
    const auto call = reinterpret_cast<int (*)()>(page);
    std::atomic<bool> running{true}, churning{false};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i)
        workers.emplace_back([&] {
            while (running) {
                ++counter;
                ct::sleep_for(.001);
            }
        });
    std::thread churn;
    std::cout << "{\"counter\":" << uintptr_t(&counter) << ",\"code\":" << uintptr_t(page) << "}\n"
              << std::flush;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "quit")
            break;
        if (command == "churn") {
            churning = true;
            churn = std::thread([&] {
                while (churning) {
                    std::thread temporary([] {
                        ++counter;
                        ct::sleep_for(.0001);
                    });
                    temporary.join();
                }
            });
            std::cout << "{\"churning\":true}\n" << std::flush;
        } else if (command == "stable") {
            churning = false;
            if (churn.joinable())
                churn.join();
            std::cout << "{\"churning\":false}\n" << std::flush;
        } else if (command == "code")
            std::cout << "{\"result\":" << call() << "}\n" << std::flush;
        else if (command == "signal")
            std::cout << "{\"signal\":" << signals << "}\n" << std::flush;
    }
    churning = false;
    if (churn.joinable())
        churn.join();
    running = false;
    for (auto &worker : workers)
        worker.join();
    munmap(page, 4096);
    return 0;
}
ct::json::Value receive(ct::ChildProcess &child) {
    const auto deadline = ct::monotonic_time() + 3;
    do {
        auto messages = child.poll();
        if (!messages.empty()) {
            assert(messages.size() == 1);
            return std::move(messages[0]);
        }
        ct::sleep_for(.001);
    } while (ct::monotonic_time() < deadline);
    throw std::runtime_error("trace test child did not reply");
}
template <class Function> void rejects(Function fn, const std::string &text) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(text) != std::string::npos;
        if (!failed)
            std::cerr << "Expected " << text << ", received " << error.what() << '\n';
    }
    assert(failed);
}
void await_state(pid_t id, char state) {
    const auto deadline = ct::monotonic_time() + 2;
    while (ct::process_stamp(ct::read_file("/proc/" + std::to_string(id) + "/stat", 16384)).state !=
           state) {
        assert(ct::monotonic_time() < deadline);
        ct::sleep_for(.001);
    }
}
size_t fd_count() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator());
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--target")
        return target();
    ct::test::ProcessImage image;
    ct::ChildProcess child({image.launch(), "--target"}, std::filesystem::current_path());
    const auto hello = receive(child);
    const auto code = uintptr_t(ct::json::unsigned_integer(ct::json::field(hello.get(), "code")));
    const auto count =
        uintptr_t(ct::json::unsigned_integer(ct::json::field(hello.get(), "counter")));
    ct::PinnedProcess process(child.pid(), image.spec(child.pid()), true);
    rejects(
        [&] {
            ct::helper::GateMemory memory(
                process, ct::helper::place_gate(ct::helper::GateKind::Input, 0x1000, 0x2000));
        },
        "unsupported native gate process");
    const auto descriptors = fd_count();
    unsigned complete_stacks = 0;
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        ct::helper::StoppedProcess stopped(process);
        assert(stopped.threads().size() >= 9);
        const auto before = process.field<uint64_t>(count);
        ct::sleep_for(.002);
        assert(process.field<uint64_t>(count) == before);
        for (auto id : stopped.threads()) {
            const auto regs = stopped.registers(id);
            assert(regs.pc());
            stopped.move_pc(id, regs.pc());
        }
        if (!cycle) {
            for (auto id : stopped.threads()) {
                const auto frames = stopped.stack(id);
                if (frames) {
                    ++complete_stacks;
                    assert(!frames->empty() && frames->front() == stopped.registers(id).pc());
                }
            }
            rejects([&] { ct::helper::StoppedProcess other(process); }, "already attached");
            rejects([&] { stopped.move_pc(stopped.threads()[0], 0); }, "outside executable");
            rejects([&] { stopped.write_code(code + 1, function(9)); }, "stopped RX");
            stopped.write_code(code + 4, {0, 0, 0, 0});
            stopped.write_code(code, function(9));
            assert(stopped.read(code, 8) == function(9));
        }
        stopped.close();
        stopped.close();
        rejects([&] { stopped.threads(); }, "not fully stopped");
    }
    assert(complete_stacks > 0);
    assert(fd_count() == descriptors);
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        const auto result =
            ct::helper::run_patch_worker(process, [&](ct::helper::StoppedProcess &stopped) {
                assert(stopped.threads().size() >= 9);
                const auto before = process.field<uint64_t>(count);
                ct::sleep_for(.001);
                assert(process.field<uint64_t>(count) == before);
                return ct::helper::PatchResult::Reused;
            });
        assert(result == ct::helper::PatchResult::Reused);
    }
    rejects(
        [&] {
            ct::helper::run_patch_worker(
                process, [](ct::helper::StoppedProcess &) -> ct::helper::PatchResult {
                    throw std::runtime_error("injected worker failure");
                });
        },
        "injected worker failure");
    rejects(
        [&] {
            ct::helper::run_patch_worker(
                process,
                [](ct::helper::StoppedProcess &) -> ct::helper::PatchResult { _exit(123); });
        },
        "did not confirm detachment");
    const auto timeout_start = ct::monotonic_time();
    rejects(
        [&] {
            ct::helper::run_patch_worker(
                process,
                [](ct::helper::StoppedProcess &) -> ct::helper::PatchResult {
                    for (;;)
                        pause();
                },
                .05);
        },
        "bounded pause");
    assert(ct::monotonic_time() - timeout_start < .5);
    {
        std::atomic<bool> waiting{true};
        std::thread other([&] {
            while (waiting)
                ct::sleep_for(.001);
        });
        rejects(
            [&] {
                ct::helper::run_patch_worker(
                    process, [](auto &) { return ct::helper::PatchResult::Reused; });
            },
            "single-threaded supervisor");
        waiting = false;
        other.join();
    }
    assert(fd_count() == descriptors);
    child.send("code\n");
    assert(ct::json::integer(ct::json::field(receive(child).get(), "result")) == 9);
    {
        ct::helper::StoppedProcess stopped(process);
        stopped.write_code(code, function(7));
        assert(!kill(child.pid(), SIGUSR1));
    }
    child.send("signal\n");
    assert(ct::json::integer(ct::json::field(receive(child).get(), "signal")) == 1);
    assert(!kill(child.pid(), SIGSTOP));
    await_state(child.pid(), 'T');
    rejects([&] { ct::helper::StoppedProcess stopped(process); }, "already stopped");
    assert(ct::process_stamp(ct::read_file(process.proc() / "stat", 16384)).state == 'T');
    assert(!kill(child.pid(), SIGCONT));
    child.send("churn\n");
    receive(child);
    for (unsigned cycle = 0; cycle < 50; ++cycle) {
        ct::helper::StoppedProcess stopped(process);
        const auto before = process.field<uint64_t>(count);
        ct::sleep_for(.001);
        assert(process.field<uint64_t>(count) == before);
    }
    child.send("stable\n");
    receive(child);
    rejects([&] { ct::helper::StoppedProcess stopped(process, .000001); }, "bounded pause");
    {
        ct::helper::StoppedProcess stopped(process);
        assert(!kill(child.pid(), SIGSTOP));
    }
    await_state(child.pid(), 'T');
    assert(!kill(child.pid(), SIGCONT));
    int channel[2];
    assert(!pipe2(channel, O_CLOEXEC));
    ct::FileDescriptor ack_read(channel[0]), ack_write(channel[1]);
    const auto worker = fork();
    assert(worker >= 0);
    if (!worker) {
        ack_read.reset();
        ct::helper::StoppedProcess stopped(process);
        assert(write(ack_write.get(), ".", 1) == 1);
        for (;;)
            pause();
    }
    ack_write.reset();
    pollfd pending{ack_read.get(), POLLIN, 0};
    assert(poll(&pending, 1, 3000) == 1 && (pending.revents & POLLIN));
    char ack = 0;
    assert(read(ack_read.get(), &ack, 1) == 1 && ack == '.');
    assert(!kill(worker, SIGKILL));
    int status = 0;
    assert(waitpid(worker, &status, 0) == worker && WIFSIGNALED(status));
    const auto before = process.field<uint64_t>(count);
    const auto deadline = ct::monotonic_time() + 2;
    while (process.field<uint64_t>(count) == before) {
        assert(ct::monotonic_time() < deadline);
        ct::sleep_for(.001);
    }
    child.send("code\n");
    assert(ct::json::integer(ct::json::field(receive(child).get(), "result")) == 7);
    {
        int pipe[2];
        assert(!pipe2(pipe, O_CLOEXEC));
        ct::FileDescriptor input(pipe[0]), output(pipe[1]);
        const auto supervisor = fork();
        assert(supervisor >= 0);
        if (!supervisor) {
            input.reset();
            ct::helper::run_patch_worker(
                process, [&](ct::helper::StoppedProcess &) -> ct::helper::PatchResult {
                    const auto id = getpid();
                    assert(write(output.get(), &id, sizeof(id)) == sizeof(id));
                    for (;;)
                        pause();
                });
            _exit(0);
        }
        output.reset();
        pollfd ready{input.get(), POLLIN, 0};
        assert(poll(&ready, 1, 3000) == 1 && (ready.revents & POLLIN));
        pid_t worker_id = 0;
        assert(read(input.get(), &worker_id, sizeof(worker_id)) == sizeof(worker_id) &&
               worker_id > 0);
        ct::FileDescriptor worker_identity(int(syscall(SYS_pidfd_open, worker_id, 0)));
        assert(worker_identity);
        assert(!kill(supervisor, SIGKILL));
        int status = 0;
        assert(waitpid(supervisor, &status, 0) == supervisor && WIFSIGNALED(status));
        pollfd exited{worker_identity.get(), POLLIN, 0};
        assert(poll(&exited, 1, 3000) == 1 && (exited.revents & POLLIN));
        const auto before = process.field<uint64_t>(count);
        const auto deadline = ct::monotonic_time() + 2;
        while (process.field<uint64_t>(count) == before) {
            assert(ct::monotonic_time() < deadline);
            ct::sleep_for(.001);
        }
    }
    child.close("quit\n");
    std::cout << "native trace: all-thread pause, register preservation, code writes, signals, "
                 "churn and tracer death passed\n";
}
