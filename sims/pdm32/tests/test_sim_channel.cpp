// The simulated device's CAN port as a can::Channel, without the device: what the simulation thread hands over comes
// out of receive() stamped with its time, what send() queues comes out of take_outbound() in order, and the edges --
// a full queue, a stopped channel, a frame classic CAN cannot carry -- are refused rather than absorbed.
#include <array>
#include <chrono>
#include <thread>

#include "mctest/test.h"
#include "pdm32/top/sim_channel.h"
#include "serve_config.h"

using sim::sys::SimCanChannel;

namespace {

helpers::CanFrame frame(uint32_t id, uint8_t len = 2) {
    helpers::CanFrame f;
    f.id = id;
    f.len = len;
    f.data[0] = 0xA5;
    return f;
}

}  // namespace

TEST(sim_channel_device_frames_come_out_of_receive_with_sim_time) {
    SimCanChannel channel("sim:test");
    channel.deliver(frame(0x500), 1234);
    channel.deliver(frame(0x501), 2000);
    std::array<helpers::CanFrame, 4> out;
    const auto got = channel.receive(out, can::Duration{10});
    CHECK(got.has_value() && *got == 2);
    CHECK_EQ(out[0].id, 0x500);
    CHECK_EQ(out[0].timestampUs, 1234);
    CHECK_EQ(out[1].id, 0x501);
    CHECK_EQ(channel.statistics().rxFrames, 2);
}

TEST(sim_channel_receive_times_out_empty) {
    SimCanChannel channel("sim:test");
    std::array<helpers::CanFrame, 4> out;
    const auto t0 = std::chrono::steady_clock::now();
    const auto got = channel.receive(out, can::Duration{50});
    CHECK(got.has_value() && *got == 0);
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(45));
}

TEST(sim_channel_stop_wakes_a_blocked_receive) {
    SimCanChannel channel("sim:test");
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)channel.stop();
    });
    std::array<helpers::CanFrame, 4> out;
    const auto t0 = std::chrono::steady_clock::now();
    (void)channel.receive(out, can::Duration{5000});
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    stopper.join();
    CHECK(!channel.running());
    CHECK(!channel.send(frame(0x100)).has_value());
}

TEST(sim_channel_sent_frames_reach_the_device_in_order) {
    SimCanChannel channel("sim:test");
    CHECK(channel.send(frame(0x100)).has_value());
    CHECK(channel.send(frame(0x101)).has_value());
    const auto outbound = channel.take_outbound();
    CHECK_EQ(outbound.size(), 2);
    CHECK(outbound.size() == 2 && outbound[0].id == 0x100 && outbound[1].id == 0x101);
    CHECK(channel.take_outbound().empty());
}

TEST(sim_channel_full_queues_drop_and_count) {
    SimCanChannel channel("sim:test", 2);
    channel.deliver(frame(1), 1);
    channel.deliver(frame(2), 2);
    channel.deliver(frame(3), 3);
    CHECK_EQ(channel.statistics().rxDropped, 1);
    std::array<helpers::CanFrame, 4> out;
    const auto got = channel.receive(out, can::Duration{10});
    CHECK(got.has_value() && *got == 2 && out[0].id == 2);  // the oldest went

    CHECK(channel.send(frame(1)).has_value());
    CHECK(channel.send(frame(2)).has_value());
    CHECK(!channel.send(frame(3)).has_value());  // the device is not taking frames: refused, not grown
    CHECK_EQ(channel.statistics().txDropped, 1);
}

TEST(sim_channel_refuses_what_classic_can_cannot_carry) {
    SimCanChannel channel("sim:test");
    helpers::CanFrame fd = frame(0x100, 12);
    fd.isFD = true;
    CHECK(!channel.send(fd).has_value());
    CHECK(!channel.send(frame(0x100, 9)).has_value());
    helpers::CanFrame wide = frame(0x800);
    CHECK(!channel.send(wide).has_value());  // 12 bits in an 11-bit identifier
    wide.isExtended = true;
    CHECK(channel.send(wide).has_value());
    CHECK(channel.set_listen_only(true).has_value());
    CHECK(!channel.send(frame(0x100)).has_value());
    CHECK(!channel.set_bitrate(can::Bitrate{}).has_value());
}

TEST(serve_config_defaults_and_overrides) {
    const auto empty = pdm32::parse_serve_config("");
    CHECK(empty.has_value());
    CHECK(empty && empty->can.rxKey == "vehicle/can0/rx" && empty->can.txKey == "vehicle/can0/tx");

    const auto named = pdm32::parse_serve_config("can: {name: can1}\nstepped: true\n");
    CHECK(named && named->can.rxKey == "vehicle/can1/rx" && named->can.txKey == "vehicle/can1/tx" && named->stepped);

    const auto keyed = pdm32::parse_serve_config("can: {rx_key: bench/pdm/rx, tx_key: bench/pdm/tx}\n");
    CHECK(keyed && keyed->can.rxKey == "bench/pdm/rx");
}

TEST(serve_config_refuses_malformed) {
    CHECK(!pdm32::parse_serve_config("can: {nmae: can1}\n").has_value());       // misspelt
    CHECK(!pdm32::parse_serve_config("scenrio: x.json\n").has_value());         // misspelt
    CHECK(!pdm32::parse_serve_config("can: can0\n").has_value());               // not a mapping
    CHECK(!pdm32::parse_serve_config("can: {rx_key: 'a/*/rx'}\n").has_value());  // not a usable key
    CHECK(!pdm32::parse_serve_config("can: {rx_key: a/b, tx_key: a/b}\n").has_value());  // hears itself
    CHECK(!pdm32::parse_serve_config("engine: turbo\n").has_value());
    CHECK(!pdm32::parse_serve_config("stepped: maybe\n").has_value());
    CHECK(!pdm32::parse_serve_config("[1, 2]\n").has_value());
    CHECK(!pdm32::parse_serve_config("can: {name: ''}\n").has_value());
}
