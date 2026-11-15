#pragma once
#include "mesh.h"
#include "interaction/pose_math.h"

namespace ct {
struct ButtonAnimation {
    static constexpr std::array<uint32_t, 14> masks{
        1, 2, 4, 8, 0x10000, 0x40, 0x4000, 0x10, 0x80000, 0x200, 0x20000, 0x80, 0x40000, 0x100};
    std::array<Point, 14> travel{};
    Point dpad_pivot{};
    std::array<Point, 4> analog_pivots{};
    std::array<std::array<float, 6>, 2> pad_mapping{};
    std::array<Matrix, 20> poses{};
    std::array<float, 18> pressed{};
    std::array<float, 8> analog{};
    double last_frame = 0;

    ButtonAnimation() {
        poses.fill(identity());
    }

    void load(std::istream &input, std::vector<Vertex> &vertices) {
        const auto read = [&](void *target, size_t size) {
            if (!input.read(static_cast<char *>(target), size))
                throw std::runtime_error("truncated button animation asset");
        };
        std::array<char, 8> magic{};
        uint32_t count = 0;
        uint64_t fingerprint = 0;
        read(magic.data(), magic.size());
        read(&count, 4);
        read(&fingerprint, 8);
        if (std::memcmp(magic.data(), "CTBT0003", 8) || size_t(count) * 3 != vertices.size() ||
            fingerprint != mesh_hash(vertices))
            throw std::runtime_error("button animation does not match controller geometry");
        read(dpad_pivot.data(), 12);
        for (float v : dpad_pivot)
            if (!std::isfinite(v) || std::abs(v) > .3f)
                throw std::runtime_error("invalid D-pad pivot");
        for (size_t part = 0; part < masks.size(); ++part) {
            uint32_t mask = 0;
            read(&mask, 4);
            read(travel[part].data(), 12);
            if (mask != masks[part])
                throw std::runtime_error("unexpected animated button mapping");
            float distance = 0;
            for (float v : travel[part]) {
                if (!std::isfinite(v))
                    throw std::runtime_error("nonfinite button travel");
                distance += v * v;
            }
            if (distance < 1e-8f || distance > 4e-6f)
                throw std::runtime_error("invalid button travel");
        }
        for (auto &pivot : analog_pivots) {
            read(pivot.data(), 12);
            for (float v : pivot)
                if (!std::isfinite(v) || std::abs(v) > .1f)
                    throw std::runtime_error("invalid analog control pivot");
        }
        for (auto &mapping : pad_mapping) {
            read(mapping.data(), sizeof(mapping));
            for (float value : mapping)
                if (!std::isfinite(value) || std::abs(value) > .1f)
                    throw std::runtime_error("invalid touchpad mapping");
            const auto width = std::hypot(mapping[2], mapping[3]);
            const auto height = std::hypot(mapping[4], mapping[5]);
            if (width < .005f || width > .025f || height < .005f || height > .025f ||
                mapping[2] <= 0 || mapping[5] >= 0 ||
                std::abs(mapping[2] * mapping[4] + mapping[3] * mapping[5]) > 1e-6f)
                throw std::runtime_error("invalid touchpad mapping axes");
        }
        std::array<uint32_t, 22> counts{};
        for (size_t triangle = 0; triangle < count; ++triangle) {
            uint8_t part = 0;
            read(&part, 1);
            if (part >= counts.size())
                throw std::runtime_error("invalid control region");
            ++counts[part];
            for (size_t corner = 0; corner < 3; ++corner) {
                auto &vertex = vertices[triangle * 3 + corner];
                vertex.part = part < poses.size() ? part : 0;
                vertex.touchpad = part >= poses.size() ? part - poses.size() + 1 : 0;
            }
        }
        for (auto n : counts)
            if (!n)
                throw std::runtime_error("missing control region");
        if (input.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("trailing button animation data");
    }

    static Matrix rock(const Point &pivot, float pitch, float roll) {
        auto rx = identity(), rz = identity();
        rx[5] = rx[10] = std::cos(pitch);
        rx[6] = std::sin(pitch);
        rx[9] = -rx[6];
        rz[0] = rz[5] = std::cos(roll);
        rz[1] = std::sin(roll);
        rz[4] = -rz[1];
        auto result = multiply(rz, rx);
        const auto rotated = transform(result, pivot);
        for (size_t axis = 0; axis < 3; ++axis)
            result[12 + axis] = pivot[axis] - rotated[axis];
        return result;
    }

    void update(uint32_t buttons, const float (&triggers)[2], const float (&left_stick)[2],
                const float (&right_stick)[2], double input_time, double now, bool visible) {
        const double dt = last_frame ? std::clamp(now - last_frame, 0., .05) : 1. / 90;
        last_frame = now;
        if (!visible || input_time <= 0 || now - input_time > .15 || now - input_time < -.02) {
            pressed.fill(0);
            analog.fill(0);
            poses.fill(identity());
            return;
        }
        const float response = float(1 - std::exp(-dt / .018));
        for (size_t i = 0; i < pressed.size(); ++i) {
            constexpr uint32_t directions[]{0x2000, 0x400, 0x1000, 0x800};
            const auto mask = i < masks.size() ? masks[i] : directions[i - masks.size()];
            const float goal = (buttons & mask) ? 1.f : 0.f;
            pressed[i] += (goal - pressed[i]) * response;
            if (std::abs(goal - pressed[i]) < .001f)
                pressed[i] = goal;
        }
        for (size_t i = 0; i < travel.size(); ++i) {
            poses[i + 1] = identity();
            for (size_t axis = 0; axis < 3; ++axis)
                poses[i + 1][12 + axis] = travel[i][axis] * pressed[i];
        }
        float pitch = pressed[15] - pressed[14], roll = pressed[16] - pressed[17];
        const float length = std::max(1.f, std::hypot(pitch, roll));
        pitch *= .045f / length;
        roll *= .045f / length;
        auto &pad = poses[15];
        pad = rock(dpad_pivot, pitch, roll);
        pad[13] -= .00015f * std::max({pressed[14], pressed[15], pressed[16], pressed[17]});

        std::array<float, 8> goals{triggers[0],
                                   triggers[1],
                                   left_stick[0],
                                   left_stick[1],
                                   right_stick[0],
                                   right_stick[1],
                                   (buttons & 0x8000) ? 1.f : 0.f,
                                   (buttons & 0x20) ? 1.f : 0.f};
        for (int side = 0; side < 2; ++side) {
            if (goals[side] < .005f)
                goals[side] = 0;
            float &x = goals[2 + side * 2];
            float &y = goals[3 + side * 2];
            const float radius = std::hypot(x, y);
            const float scale =
                radius > .025f ? std::min(1.f, (radius - .025f) / .975f) / radius : 0;
            x *= scale;
            y *= scale;
        }
        for (size_t i = 0; i < analog.size(); ++i) {
            analog[i] += (goals[i] - analog[i]) * response;
            if (std::abs(goals[i] - analog[i]) < .0001f)
                analog[i] = goals[i];
        }
        for (int side = 0; side < 2; ++side) {
            poses[16 + side] = rock(analog_pivots[side], -.30f * analog[side], 0);
            auto &stick = poses[18 + side];
            stick = rock(analog_pivots[2 + side], -.35f * analog[3 + side * 2],
                         -.35f * analog[2 + side * 2]);
            for (int axis = 0; axis < 3; ++axis)
                stick[12 + axis] -= stick[4 + axis] * .00065f * analog[6 + side];
        }
    }
};
} // namespace ct
