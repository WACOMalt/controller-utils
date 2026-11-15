#include "input_gates.h"
#include "discovery.h"
#include "worker.h"
#include "platform/linux/files.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <system_error>

namespace ct::helper {
namespace {
bool unavailable(const std::exception &error) {
    const auto system = dynamic_cast<const std::system_error *>(&error);
    return dynamic_cast<const ConnectionError *>(&error) ||
           (system && (system->code() == std::errc::no_such_file_or_directory ||
                       system->code() == std::errc::no_such_process));
}
class NativeGate final : public InputGates::Owner {
    GateKind kind_;
    PinnedProcess process_;
    GateLayout layout_;
    std::optional<GateMemory> memory_;
    std::optional<GateLease> lease_;
    bool attempted_ = false;
    static ProcessSpec spec(GateKind kind, pid_t pid) {
        const auto &binary = kind == GateKind::Input ? steamvr::compatibility::SteamWebHelper
                                                     : steamvr::compatibility::Compositor;
        return {binary.path, sha256("/proc/" + std::to_string(pid) + "/exe"), 1000, std::nullopt};
    }
    static pid_t pid(GateKind kind) {
        return find_process(kind == GateKind::Input ? steamvr::compatibility::SteamWebHelper
                                                    : steamvr::compatibility::Compositor,
                            kind == GateKind::Input);
    }

    NativeGate(GateKind kind, pid_t pid, bool restoring)
        : kind_(kind), process_(pid, spec(kind, pid), true),
          layout_(resolve_gate_layout(kind, process_.image())) {
        const auto &layout = layout_;
        validate_gate_mappings(layout, process_.image_base(), process_.mappings());
        const auto cave = process_.read(process_.image_base() + layout.cave, layout.payload.size());
        const auto live = process_.read(process_.image_base() + layout.site, 12);
        validate_gate_start(layout, live, cave, restoring);
    }

  public:
    NativeGate(GateKind kind, bool restoring) : NativeGate(kind, pid(kind), restoring) {}
    bool alive() const {
        return process_.alive();
    }
    uint32_t index() const override {
        return lease_ ? lease_->index() : UINT32_MAX;
    }
    void apply() override {
        attempted_ = true;
        patch_gate(process_, layout_, true);
        memory_.emplace(process_, layout_);
        lease_.emplace(*memory_, layout_, process_.image_base());
    }
    void renew(uint32_t index, double now) override {
        if (!lease_)
            throw std::runtime_error("Steam UI input gate is unavailable.");
        lease_->renew(index, now);
    }
    void disable() override {
        if (lease_)
            lease_->disable();
    }
    void poll(double now) override {
        if (!alive())
            throw ConnectionError(
                "Steam UI restarted. UI control is unavailable until tracking restarts.");
        if (lease_)
            lease_->poll(now);
    }
    void restore(bool force) override {
        if ((!attempted_ && !force) || !alive())
            return;
        process_.refresh();
        const auto &layout = layout_;
        const auto site = process_.read(process_.image_base() + layout.site, 4);
        const auto cave = process_.read(process_.image_base() + layout.cave, layout.payload.size());
        if (site == layout.original &&
            (cave == Bytes(cave.size()) || (kind_ == GateKind::Beam && cave == layout.payload)))
            return;
        try {
            disable();
        } catch (const std::exception &) {
        }
        try {
            patch_gate(process_, layout_, false);
        } catch (const std::exception &) {
            sleep_for(.05);
            patch_gate(process_, layout_, false);
        }
        lease_.reset();
        memory_.reset();
        attempted_ = false;
    }
};
} // namespace
InputGates::InputGates(TrackingBackend &backend, Factory factory)
    : backend_(backend),
      factory_(factory ? std::move(factory) : Factory([](GateKind kind, bool restoring) {
          return std::make_unique<NativeGate>(kind, restoring);
      })) {}
InputGates::~InputGates() = default;
void InputGates::error(const char *operation, const std::string &text) {
    auto value = json::parse("{}");
    json_object_object_add(value.get(), "op", json_object_new_string(operation));
    json_object_object_add(
        value.get(), "message",
        json_object_new_string_len(text.data(), int(std::min(size_t(1000), text.size()))));
    output_.emit(value.get());
}
void InputGates::state(const char *operation, uint32_t index) {
    auto value = json::parse("{}");
    json_object_object_add(value.get(), "op", json_object_new_string(operation));
    json_object_object_add(value.get(), "index",
                           json_object_new_int(index == UINT32_MAX ? -1 : int(index)));
    if (std::string(operation) == "input_gate_state")
        json_object_object_add(value.get(), "monotonic_time",
                               json_object_new_double(monotonic_time()));
    output_.emit(value.get());
}
void InputGates::close(std::unique_ptr<Owner> &owner, const char *operation, bool force) {
    if (!owner)
        return;
    try {
        owner->restore(force);
    } catch (const std::exception &failure) {
        error(operation, failure.what());
        throw;
    }
    owner.reset();
}
void InputGates::acquire(GateKind kind, double now) {
    auto &retry = kind == GateKind::Input ? input_retry_ : beam_retry_;
    if (!retry || now < *retry)
        return;
    auto &owner = kind == GateKind::Input ? input_ : beam_;
    auto &message = kind == GateKind::Input ? input_error_ : beam_error_;
    const auto failure = kind == GateKind::Input ? "input_gate_error" : "beam_gate_error";
    try {
        owner = factory_(kind, false);
        if (!owner)
            throw std::runtime_error("Native UI gate could not be created.");
    } catch (const std::exception &problem) {
        if (unavailable(problem)) {
            retry = now + 1;
            if (message != problem.what()) {
                message = problem.what();
                error(kind == GateKind::Input ? "input_gate_waiting" : "beam_gate_waiting",
                      message);
            }
        } else {
            retry.reset();
            error(failure, problem.what());
        }
        return;
    }
    retry.reset();
    try {
        owner->apply();
    } catch (const std::exception &problem) {
        const std::string text = problem.what();
        close(owner, failure);
        error(failure, text);
        return;
    }
    message.clear();
    const auto ready = json::parse(kind == GateKind::Input ? "{\"op\":\"input_gate_ready\"}"
                                                           : "{\"op\":\"beam_gate_ready\"}");
    output_.emit(ready.get());
    if (kind == GateKind::Input)
        state("input_gate_state", owner->index());
}
void InputGates::start(const RecoveryOutput &) {
    started_ = true;
}
void InputGates::command(uint8_t value, double now) {
    if (!input_)
        return;
    if ((value >= 0x80 && value < 0xc0) || value == 'v') {
        if (!beam_)
            return;
        try {
            const auto before = beam_->index();
            if (value == 'v')
                beam_->disable();
            else
                beam_->renew(value - 0x80, now);
            if (beam_->index() != before)
                state("beam_gate_state", beam_->index());
        } catch (const std::exception &failure) {
            const std::string text = failure.what();
            try {
                close(beam_, "beam_gate_error");
            } catch (const std::exception &) {
            }
            error("beam_gate_error", text);
        }
        return;
    }
    try {
        const auto before = input_->index();
        if (value >= 'A' && value <= 'P') {
            input_->renew(value - 'A', now);
            state("input_gate_state", input_->index());
        } else if (value == '-') {
            if (beam_)
                beam_->disable();
            input_->disable();
            if (input_->index() != before)
                state("input_gate_state", input_->index());
        }
    } catch (const std::exception &failure) {
        const std::string text = failure.what();
        try {
            release_input();
        } catch (const std::exception &) {
        }
        error("input_gate_error", text);
    }
}
void InputGates::poll(double now) {
    acquire(GateKind::Input, now);
    if (input_)
        acquire(GateKind::Beam, now);
    if (input_) {
        try {
            const auto before = input_->index();
            input_->poll(now);
            if (input_->index() != before)
                state("input_gate_state", input_->index());
        } catch (const std::exception &failure) {
            const std::string text = failure.what();
            try {
                release_input();
            } catch (const std::exception &) {
            }
            error("input_gate_error", text);
        }
    }
    if (beam_) {
        try {
            beam_->poll(now);
        } catch (const std::exception &failure) {
            const std::string text = failure.what();
            try {
                close(beam_, "beam_gate_error");
            } catch (const std::exception &) {
            }
            error("beam_gate_error", text);
        }
    }
}
void InputGates::release_input() {
    input_retry_.reset();
    beam_retry_.reset();
    std::exception_ptr failure;
    if (!started_) {
        // The guard may have acquired its gates after the supervisor forked.
        for (const auto kind : {GateKind::Beam, GateKind::Input}) {
            auto &owner = kind == GateKind::Input ? input_ : beam_;
            try {
                owner = factory_(kind, true);
            } catch (const std::exception &problem) {
                if (!unavailable(problem) && !failure)
                    failure = std::current_exception();
            }
        }
    }
    try {
        close(beam_, "beam_gate_error", true);
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        close(input_, "input_gate_error", true);
    } catch (...) {
        if (!failure)
            failure = std::current_exception();
    }
    if (failure)
        std::rethrow_exception(failure);
}
void InputGates::restore_tracking() {
    backend_.restore();
}
} // namespace ct::helper
