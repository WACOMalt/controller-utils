#include "interaction/pointer_math.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    const auto near = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
    bool invalid = false;

    ct::PadMotionFilter pad;
    const std::array<float, 2> still{};
    assert(pad.update(.2f, -.3f, 1, true) == still);
    for (int sample = 1; sample <= 1000; ++sample) {
        const float angle = sample * .7f;
        assert(pad.update(.2f + .003f * std::cos(angle), -.3f + .003f * std::sin(angle),
                          1 + sample * .01, true) == still);
    }
    assert(pad.update(-.8f, .8f, 12, false) == still);
    assert(pad.update(-.8f, .8f, 13, true) == still);
    pad.reset();
    assert(pad.update(.7f, -.4f, 14, true) == still);
    assert(pad.update(-.7f, .4f, 14, true) == still);
    assert(pad.update(-.7f, .4f, 13, true) == still);
    assert(pad.update(.7f, -.4f, 14.01, true) == still);
    invalid = false;
    try {
        (void)pad.update(std::numeric_limits<float>::quiet_NaN(), 0, 15, true);
    } catch (const std::runtime_error &) {
        invalid = true;
    }
    assert(invalid && pad.update(.7f, -.4f, 15, true) == still);

    pad.reset();
    (void)pad.update(0, 0, 0, true);
    float total_pad_motion = 0;
    for (int sample = 1; sample <= 100; ++sample)
        total_pad_motion += pad.update(sample * .0005f, 0, sample * .01, true)[0];
    for (int sample = 101; sample <= 120; ++sample)
        total_pad_motion += pad.update(.05f, 0, sample * .01, true)[0];
    assert(near(total_pad_motion, .05f - ct::PadMotionFilter::quiet_radius));
    for (int rate : {60, 90, 144, 250}) {
        pad.reset();
        (void)pad.update(0, 0, 0, true);
        std::array<float, 2> total{};
        for (int sample = 1;; ++sample) {
            const double time = std::min(.024, double(sample) / rate);
            const auto delta = pad.update(.12f, .16f, time, true);
            total[0] += delta[0];
            total[1] += delta[1];
            assert(pad.update(.12f, .16f, time, true) == still);
            if (time == .024)
                break;
        }
        const float travel = (.2f - ct::PadMotionFilter::quiet_radius) * (1 - std::exp(-2.f));
        assert(near(total[0], travel * .6f) && near(total[1], travel * .8f));
    }
    pad.reset();
    (void)pad.update(0, 0, 0, true);
    const auto prompt_motion = pad.update(.1f, 0, .012, true);
    assert(prompt_motion[0] > .05f && prompt_motion[0] < .1f);
    float bounded_motion = prompt_motion[0];
    for (int sample = 2; sample < 1000; ++sample) {
        const float noise = sample % 2 ? .002f : -.002f;
        const auto delta = pad.update(.1f + noise, 0, sample * .012, true);
        bounded_motion += delta[0];
        if (sample > 100)
            assert(delta == still);
    }
    assert(bounded_motion > .09f && bounded_motion < .1f);

    std::cout << "pointer math: quiet touchpad passed\n";
}
