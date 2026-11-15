#include "controller/reader.h"
#include "fake_controller.h"
#include <deque>

namespace {
struct State {
    std::shared_ptr<ct::test::FakeState> features = std::make_shared<ct::test::FakeState>();
    std::deque<std::vector<uint8_t>> packets;
    unsigned reads = 0, fail_read = 0;
};
class Transport final : public ct::HidTransport {
    std::shared_ptr<State> state_;
    ct::test::FakeTransport features_;

  public:
    explicit Transport(std::shared_ptr<State> state)
        : state_(std::move(state)), features_(state_->features) {}
    void send(const std::array<uint8_t, 64> &report) override {
        features_.send(report);
    }
    std::array<uint8_t, 64> feature() override {
        return features_.feature();
    }
    std::optional<std::vector<uint8_t>> receive(double timeout) override {
        assert(timeout == 0);
        if (++state_->reads == state_->fail_read)
            throw std::system_error(ENODEV, std::generic_category());
        if (state_->packets.empty())
            return std::nullopt;
        state_->features->time += .001;
        auto packet = std::move(state_->packets.front());
        state_->packets.pop_front();
        return packet;
    }
    double now() override {
        return features_.now();
    }
    void wait(double seconds) override {
        features_.wait(seconds);
    }
    void close() noexcept override {
        features_.close();
    }
};
std::vector<uint8_t> packet(uint32_t tick) {
    std::vector<uint8_t> bytes(46);
    bytes[0] = 0x45;
    for (unsigned i = 0; i < 4; ++i)
        bytes[30 + i] = uint8_t(tick >> (8 * i));
    return bytes;
}
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace
int main() {
    {
        double now = 0;
        unsigned opens = 0;
        auto state = std::make_shared<State>();
        state->features->feature_misses = 20;
        ct::ControllerReader reader(
            [&] {
                ++opens;
                return std::make_shared<ct::ControllerDevice>(std::make_unique<Transport>(state));
            },
            [&] { return now; });
        assert(reader.poll(true).empty() && !reader.connected());
        assert(opens == 1 && state->features->feature_reads == 20);
        for (unsigned tick = 0; tick < 1000; ++tick)
            assert(reader.poll(true).empty());
        assert(opens == 1 && state->features->feature_reads == 20);
        now = 1;
        assert(reader.poll(true).empty() && reader.connected());
        assert(opens == 2 && state->features->feature_reads == 21);
        assert(reader.device()->serial() == "TEST1234");
        assert(state->features->feature_reads == 21);
    }
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        double now = 100;
        unsigned opens = 0;
        auto state = std::make_shared<State>();
        state->features->time = now;
        ct::ControllerReader reader(
            [&] {
                ++opens;
                return std::make_shared<ct::ControllerDevice>(std::make_unique<Transport>(state));
            },
            [&] { return now; });
        assert(reader.poll(false).empty() && !reader.connected() && opens == 0);
        for (uint32_t tick = 0; tick < 300; ++tick)
            state->packets.push_back(packet(tick));
        auto first = reader.poll(true), second = reader.poll(true), third = reader.poll(true);
        assert(first.size() == 128 && second.size() == 128 && third.size() == 44);
        double previous = 100;
        uint64_t tick = 0;
        for (const auto &batch : {first, second, third})
            for (const auto &input : batch) {
                assert(input.report.sensor_tick == tick++);
                assert(input.received > previous);
                previous = input.received;
            }
        assert(reader.connected() && opens == 1 && !state->features->closed);
        auto session_device = reader.device();
        session_device->enable_ir(100);
        state->packets.push_back(packet(301));
        state->fail_read = state->reads + 2;
        assert(reader.poll(true).empty() && !reader.connected());
        assert(!state->features->closed && state->features->brightness == 100);
        session_device->restore();
        assert(state->features->brightness == 25);
        session_device.reset();
        assert(state->features->closed == 1);
        assert(reader.poll(true).empty() && opens == 1);
        now += 1;
        assert(reader.poll(true).empty() && opens == 2 && reader.connected());
        reader.close();
        reader.close();
        assert(state->features->closed == 2);
    }
    {
        unsigned attempts = 0;
        double now = 0;
        ct::ControllerReader reader(
            [&]() -> std::shared_ptr<ct::ControllerDevice> {
                ++attempts;
                throw std::runtime_error("controller absent");
            },
            [&] { return now; });
        assert(reader.poll(true).empty() && attempts == 1);
        now = .99;
        assert(reader.poll(true).empty() && attempts == 1);
        now = 1;
        assert(reader.poll(true).empty() && attempts == 2 && !reader.connected());
        assert(reader.poll(false).empty() && attempts == 2);
        ct::ControllerReader invalid([]() -> std::shared_ptr<ct::ControllerDevice> {
            throw std::invalid_argument("invalid identity");
        });
        rejects([&] { invalid.poll(true); });
        ct::ControllerReader missing([] { return std::shared_ptr<ct::ControllerDevice>(); });
        assert(missing.poll(true).empty() && !missing.connected());
    }
    {
        auto state = std::make_shared<State>();
        ct::ControllerReader reader([&] {
            return std::make_shared<ct::ControllerDevice>(std::make_unique<Transport>(state));
        });
        state->packets.push_back({0x45});
        rejects([&] { reader.poll(true); });
        reader.close();
        assert(state->features->closed == 1);
    }
    std::cout << "controller reader: ordered bounded batches, reconnect and 100 shared-handle "
                 "lifecycles passed\n";
}
