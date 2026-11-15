#include "input_gates.h"
#include "discovery.h"
#include "worker.h"
#include "platform/linux/files.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <exception>
#include <stdexcept>

namespace ct::helper {
class InputGates::Owner {
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

    Owner(GateKind kind, pid_t pid)
        : kind_(kind), process_(pid, spec(kind, pid), true),
          layout_(resolve_gate_layout(kind, process_.image())) {
        const auto &layout = layout_;
        validate_gate_mappings(layout, process_.image_base(), process_.mappings());
        auto site = layout.original;
        site.insert(site.end(), layout.following.begin(), layout.following.end());
        const auto cave = process_.read(process_.image_base() + layout.cave, layout.payload.size());
        if (process_.read(process_.image_base() + layout.site, 12) != site ||
            (cave != Bytes(cave.size()) && !(kind == GateKind::Beam && cave == layout.payload)))
            throw std::runtime_error("Steam UI input code differs from the supported original.");
    }

  public:
    explicit Owner(GateKind kind) : Owner(kind, pid(kind)) {}
    bool alive() const {
        return process_.alive();
    }
    uint32_t index() const {
        return lease_ ? lease_->index() : UINT32_MAX;
    }
    void apply() {
        attempted_ = true;
        patch_gate(process_, layout_, true);
        memory_.emplace(process_, layout_);
        lease_.emplace(*memory_, layout_, process_.image_base());
    }
    void renew(uint32_t index, double now) {
        if (!lease_)
            throw std::runtime_error("Steam UI input gate is unavailable.");
        lease_->renew(index, now);
    }
    void disable() {
        if (lease_)
            lease_->disable();
    }
    void poll(double now) {
        if (!alive())
            throw ConnectionError(
                "Steam UI restarted. UI control is unavailable until tracking restarts.");
        if (lease_)
            lease_->poll(now);
    }
    void restore(bool force) {
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
InputGates::InputGates(TrackingBackend &backend) : backend_(backend) {
    try {
        input_ = std::make_unique<Owner>(GateKind::Input);
    } catch (const std::exception &error) {
        input_error_ = error.what();
    }
    if (input_) {
        try {
            beam_ = std::make_unique<Owner>(GateKind::Beam);
        } catch (const std::exception &error) {
            beam_error_ = error.what();
        }
    }
}
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
void InputGates::start(const RecoveryOutput &) {
    if (!input_) {
        error("input_gate_error", input_error_);
        return;
    }
    try {
        input_->apply();
    } catch (const std::exception &failure) {
        const std::string text = failure.what();
        try {
            close(input_, "input_gate_error");
        } catch (const std::exception &) {
        }
        error("input_gate_error", text);
        return;
    }
    if (beam_) {
        try {
            beam_->apply();
            const auto ready = json::parse("{\"op\":\"beam_gate_ready\"}");
            output_.emit(ready.get());
        } catch (const std::exception &failure) {
            const std::string text = failure.what();
            try {
                close(beam_, "beam_gate_error");
            } catch (const std::exception &) {
            }
            error("beam_gate_error", text);
        }
    } else
        error("beam_gate_error", beam_error_);
    const auto ready = json::parse("{\"op\":\"input_gate_ready\"}");
    output_.emit(ready.get());
    state("input_gate_state", input_->index());
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
    std::exception_ptr failure;
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
