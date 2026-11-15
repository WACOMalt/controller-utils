#include "helper/input_gates.h"
#include "platform/linux/error.h"
#include <cassert>
#include <cerrno>
#include <iostream>
#include <system_error>

namespace {
struct State {
    bool available = true, invalid = false, apply_failure = false, restore_failure = false;
    unsigned acquired = 0, recovery = 0, applied = 0, restored = 0;
    int lookup_error = 0;
    uint32_t index = UINT32_MAX;
};
class Gate final : public ct::helper::InputGates::Owner {
    State &state_;

  public:
    explicit Gate(State &state) : state_(state) {}
    uint32_t index() const override {
        return state_.index;
    }
    void apply() override {
        ++state_.applied;
        if (state_.apply_failure)
            throw ct::ConnectionError("injected patch failure");
    }
    void renew(uint32_t index, double) override {
        state_.index = index;
    }
    void disable() override {
        state_.index = UINT32_MAX;
    }
    void poll(double) override {}
    void restore(bool) override {
        ++state_.restored;
        if (state_.restore_failure)
            throw std::runtime_error("injected cleanup failure");
        disable();
    }
};
class Backend final : public ct::helper::TrackingBackend {
  public:
    unsigned restores = 0;
    int descriptor() const override {
        return -1;
    }
    bool start() override {
        assert(false);
        return false;
    }
    void renew() override {
        assert(false);
    }
    std::vector<ct::json::Value> poll(bool) override {
        assert(false);
        return {};
    }
    void restore(const std::function<void()> &) override {
        ++restores;
    }
};
struct Fixture {
    Backend backend;
    State input, beam;
    ct::helper::InputGates gates{
        backend, [&](ct::helper::GateKind kind, bool restoring) {
            auto &state = kind == ct::helper::GateKind::Input ? input : beam;
            if (restoring)
                ++state.recovery;
            else
                ++state.acquired;
            if (!state.available)
                throw ct::ConnectionError("waiting for Steam");
            if (state.lookup_error)
                throw std::system_error(state.lookup_error, std::generic_category());
            if (state.invalid)
                throw std::runtime_error("incompatible Steam interface");
            return std::make_unique<Gate>(state);
        }};
    double now = 100;
    void start() {
        gates.start(ct::helper::RecoveryOutput(-1));
        gates.poll(now);
    }
    void poll(double elapsed = 1.1) {
        gates.poll(now += elapsed);
    }
};
} // namespace
int main() {
    for (const auto code : {ENOENT, ESRCH, EACCES}) {
        Fixture f;
        f.input.lookup_error = code;
        f.start();
        f.input.lookup_error = 0;
        f.poll();
        assert(f.input.applied == unsigned(code != EACCES));
        assert(f.input.acquired == (code == EACCES ? 1u : 2u));
        f.gates.release_input();
        Fixture cleanup;
        cleanup.input.lookup_error = code;
        bool failed = false;
        try {
            cleanup.gates.release_input();
        } catch (const std::exception &) {
            failed = true;
        }
        assert(failed == (code == EACCES));
        assert(cleanup.beam.restored == 1 && !cleanup.input.applied);
    }
    {
        Fixture f;
        f.input.available = false;
        f.start();
        assert(f.input.acquired == 1 && !f.input.applied && !f.beam.acquired);
        f.poll(.1);
        assert(f.input.acquired == 1);
        f.input.available = true;
        f.poll();
        assert(f.input.acquired == 2 && f.input.applied == 1 && f.beam.applied == 1);
        f.gates.command('F', f.now);
        f.gates.command(0x85, f.now);
        assert(f.input.index == 5 && f.beam.index == 5);
        f.poll();
        assert(f.input.acquired == 2 && f.beam.acquired == 1 && !f.backend.restores);
        f.gates.release_input();
        f.poll();
        assert(f.input.index == UINT32_MAX && f.beam.index == UINT32_MAX);
        assert(f.input.restored == 1 && f.beam.restored == 1 && !f.backend.restores);
        assert(f.input.acquired == 2 && f.beam.acquired == 1);
    }
    {
        Fixture f;
        f.beam.available = false;
        f.start();
        assert(f.input.applied == 1 && !f.beam.applied);
        f.gates.command('C', f.now);
        assert(f.input.index == 2);
        f.beam.available = true;
        f.poll();
        assert(f.input.acquired == 1 && f.beam.acquired == 2 && f.beam.applied == 1);
        f.gates.release_input();
    }
    {
        Fixture f;
        f.input.invalid = true;
        f.start();
        f.input.invalid = false;
        f.poll(100);
        assert(f.input.acquired == 1 && !f.input.applied && !f.beam.acquired);
        f.gates.release_input();
    }
    {
        Fixture f;
        f.input.apply_failure = true;
        f.start();
        f.poll(100);
        assert(f.input.acquired == 1 && f.input.applied == 1 && f.input.restored == 1);
        f.gates.release_input();
    }
    {
        Fixture f;
        f.input.available = false;
        f.start();
        f.gates.release_input();
        f.input.available = true;
        f.poll(100);
        assert(f.input.acquired == 1 && !f.input.applied && !f.input.recovery);
    }
    {
        Fixture f;
        f.gates.release_input();
        assert(f.input.recovery == 1 && f.beam.recovery == 1);
        assert(!f.input.applied && !f.beam.applied);
        assert(f.input.restored == 1 && f.beam.restored == 1 && !f.backend.restores);
    }
    {
        Fixture f;
        f.beam.available = false;
        f.input.restore_failure = true;
        bool failed = false;
        try {
            f.gates.release_input();
        } catch (const std::exception &) {
            failed = true;
        }
        assert(failed && f.input.restored == 1 && f.beam.recovery == 1);
        f.input.restore_failure = false;
        f.gates.release_input();
    }
    std::cout
        << "UI gates: deferred startup, bounded retries, incompatibility refusal and recovery "
           "passed\n";
}
