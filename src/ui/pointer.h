#pragma once

struct ControllerPointer {
    Vr &vr;
    Dashboard &dashboard;
    double input_time = 0, processed_time = 0, keyboard_time = 0;
    double grip_time = 0;
    uint64_t grip_known = 0, grip_down = 0;
    uint32_t buttons = 0, previous_binding = 0;
    float triggers[2]{}, left[3]{}, right[3]{}, stick[2]{}, left_stick[2]{};
    bool enabled = false, binding_down = false, keyboard_typing = false;
    explicit ControllerPointer(Vr &runtime, Dashboard &panel) : vr(runtime), dashboard(panel) {}
    bool accept(const ct::ui::Command &command) {
        if (const auto op = std::get_if<ct::ui::Operation>(&command)) {
            if (*op == ct::ui::Operation::InputToggle) {
                set_enabled(!enabled);
                return true;
            }
            if (*op == ct::ui::Operation::InputOff) {
                set_enabled(false);
                dashboard.dirty |= dashboard.hovered != nullptr || dashboard.pressed != nullptr;
                dashboard.hovered = dashboard.pressed = nullptr;
                return true;
            }
            return false;
        }
        if (const auto next = std::get_if<ct::ui::KeyboardTyping>(&command)) {
            keyboard_typing = next->active;
            return true;
        }
        const auto next = std::get_if<ct::ui::Controls>(&command);
        if (!next)
            return false;
        if (next->time <= input_time)
            throw std::runtime_error("invalid controller input time or buttons");
        std::copy(next->triggers.begin(), next->triggers.end(), triggers);
        std::copy(next->left_pad.begin(), next->left_pad.end(), left);
        std::copy(next->right_pad.begin(), next->right_pad.end(), right);
        std::copy(next->left_stick.begin(), next->left_stick.end(), left_stick);
        std::copy(next->right_stick.begin(), next->right_stick.end(), stick);
        input_time = next->time;
        buttons = next->buttons;
        return true;
    }
    void set_enabled(bool value) {
        if (enabled != value) {
            grip_known = grip_down = 0;
            dashboard.emit(ct::ui::PointerEnabled{value});
        }
        enabled = value;
        dashboard.dirty |= dashboard.input_enabled != value;
        dashboard.input_enabled = value;
    }
    void grips(const ct::steamvr::GripState &state) {
        const auto pressed = state.down & state.valid & grip_known & ~grip_down;
        grip_known = state.valid;
        grip_down = state.down & state.valid;
        if (enabled && pressed)
            set_enabled(false);
    }
    bool toggle(double now, uint32_t binding) {
        if (now - input_time > .1 || now - input_time < -.02) {
            set_enabled(false);
            binding_down = true;
            return false;
        }
        if (processed_time != input_time) {
            const bool down = (buttons & binding) == binding;
            if (previous_binding != binding)
                binding_down = down;
            if (down && !binding_down)
                set_enabled(!enabled);
            binding_down = down;
            previous_binding = binding;
            processed_time = input_time;
        }
        return enabled;
    }
    void show_input_state(const std::string &value) {
        dashboard.dirty |= dashboard.input_state != value;
        dashboard.input_state = value;
    }
    void keyboard_state(const std::optional<ct::Matrix> &head, double now) {
        if (now - keyboard_time < .05)
            return;
        keyboard_time = now;
        vr::VROverlayHandle_t keyboard = 0;
        const bool visible =
            !vr.overlay->FindOverlay("valve.steam.gamepadui.keyboard", &keyboard) &&
            vr.overlay->IsOverlayVisible(keyboard);
        vr::HmdVector2_t ndc[2]{};
        const bool valid =
            visible && enabled && head && vr.system->GetEyeTrackedFoveationCenter(&ndc[0], &ndc[1]);
        bool hit = false;
        if (valid)
            for (int i = 0; i < 2; ++i) {
                const auto eye = static_cast<vr::EVREye>(i);
                const auto ray = ct::gaze_ray(
                    ndc[i], vr.system->GetProjectionMatrix(eye, .05f, 50),
                    ct::multiply(*head, matrix(vr.system->GetEyeToHeadTransform(eye))));
                if (!ray)
                    continue;
                vr::VROverlayIntersectionParams_t query{};
                query.eOrigin = vr::TrackingUniverseStanding;
                for (int axis = 0; axis < 3; ++axis) {
                    query.vSource.v[axis] = ray->origin[axis];
                    query.vDirection.v[axis] = ray->direction[axis];
                }
                vr::VROverlayIntersectionResults_t result{};
                hit |= vr.overlay->ComputeOverlayIntersection(keyboard, &query, &result) &&
                       std::isfinite(result.fDistance) && result.fDistance > 0 &&
                       result.fDistance < 10;
            }
        dashboard.emit(ct::ui::KeyboardState{now, visible, valid, hit});
    }
    void update(const std::optional<ct::Matrix> &head) {
        const auto now = native_time();
        if (!head || dashboard.capturing)
            set_enabled(false);
        else
            toggle(now, dashboard.pointer_buttons);
        if (enabled && now - grip_time >= .02) {
            grip_time = now;
            grips(vr.controller_input.grips(*vr.system));
        }
        keyboard_state(head, now);
        show_input_state(enabled ? (keyboard_typing ? "Typing with both pads" : "Aim or swipe")
                                 : "Off");
    }
};
