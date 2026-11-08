// MSCAN08: initialisation, transmit timing and order, acceptance filters, the double-buffered receiver, sleep and
// wake-up.
#include <vector>

#include "gz60/soc.h"
#include "mctest/test.h"

using namespace sim;
using namespace sim::sys;

namespace {

struct Tester final : CanNode {
    std::vector<std::pair<CanFrame, uint64_t>> got;
    bool receive(const CanFrame& frame, const CanNode&, uint64_t now) override {
        got.emplace_back(frame, now);
        return true;
    }
};

struct Bench {
    Gz60 soc;
    CanBus bus;
    Tester tester;
    Bench() {
        soc.m.mem.bytes[0xFFFE] = 0x80;
        soc.m.mem.bytes[0xFFFF] = 0x00;
        soc.m.mem.bytes[0x8000] = 0x20;
        soc.m.mem.bytes[0x8001] = 0xFE;
        soc.m.reset(ResetCause::PowerOn);
        soc.cgm->set_bootloader_state();
        soc.cop->set_enabled(false);
        soc.mscan->attach(bus);
        bus.attach(tester);
        // as the firmware initialises it: soft reset, CGMOUT, 1 Mbit/s, accept everything
        w(0x0500, 0x01);
        w(0x0501, 0x01);
        w(0x0502, 0x01);
        w(0x0503, 0x14);
        for (int k = 0; k < 4; k++) {
            w(uint16_t(0x0514 + k), 0xFF);
        }
        w(0x0500, 0x00);
    }
    void w(uint16_t a, uint8_t v) {
        soc.m.write(a, v);
    }
    uint8_t r(uint16_t a) {
        return soc.m.read(a);
    }
    void run_us(double us) {
        soc.m.run_until(soc.m.now() + uint64_t(us * 32));
    }
};

}  // namespace

TEST(mscan_bit_rate_from_the_firmware_setting) {
    Bench b;
    CHECK_EQ(b.soc.mscan->bit_rate(), 1'000'000);
    CHECK((b.r(0x0500) & 0x10) != 0);  // SYNCH
}

TEST(mscan_transmit_takes_frame_time_and_sets_txe) {
    Bench b;
    // TX0: id 0x504, 8 bytes
    b.w(0x0550, 0x504 >> 3);
    b.w(0x0551, (0x504 & 7) << 5);
    for (int k = 0; k < 8; k++) {
        b.w(uint16_t(0x0554 + k), uint8_t(k + 1));
    }
    b.w(0x055C, 8);
    b.w(0x0507, 0x01);  // TXEIE0
    b.w(0x0506, 0x01);  // send
    CHECK_EQ(b.r(0x0506) & 1, 0);
    b.run_us(100);  // 111 bits at 1 us = 111 us
    CHECK(b.tester.got.empty());
    b.run_us(15);
    CHECK_EQ(b.tester.got.size(), 1);
    CHECK_EQ(b.tester.got[0].first.id, 0x504);
    CHECK_EQ(b.tester.got[0].first.data[7], 8);
    CHECK_EQ(b.r(0x0506) & 1, 1);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kMscanTransmit)) != 0);
}

TEST(mscan_receive_filter_and_double_buffer) {
    Bench b;
    // 8-bit filters on IDR0 = 0x3E0 >> 3 only
    b.w(0x0500, 0x01);
    b.w(0x0508, 0x20);
    for (int k = 0; k < 4; k++) {
        b.w(uint16_t(0x0510 + k), 0x3E0 >> 3);
        b.w(uint16_t(0x0514 + k), 0x00);
    }
    b.w(0x0500, 0x00);
    b.w(0x0505, 0x01);  // RXFIE

    CanFrame other;
    other.id = 0x123;
    other.len = 1;
    b.bus.send(b.tester, other, b.soc.m.now());
    CHECK_EQ(b.r(0x0504) & 1, 0);

    CanFrame f1;
    f1.id = 0x3E1;
    f1.len = 2;
    f1.data = {0xAA, 0xBB};
    CanFrame f2 = f1;
    f2.data[0] = 0xCC;
    b.bus.send(b.tester, f1, b.soc.m.now());
    b.bus.send(b.tester, f2, b.soc.m.now());
    CHECK_EQ(b.r(0x0504) & 1, 1);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kMscanReceive)) != 0);
    CHECK_EQ(b.r(0x0544), 0xAA);
    b.w(0x0504, 0x01);  // release: the background frame moves up
    CHECK_EQ(b.r(0x0504) & 1, 1);
    CHECK_EQ(b.r(0x0544), 0xCC);
    b.w(0x0504, 0x01);
    CHECK_EQ(b.r(0x0504) & 1, 0);
}

TEST(mscan_sleep_and_wakeup_on_bus_activity) {
    Bench b;
    b.w(0x0500, 0x02);                 // SLPRQ
    CHECK((b.r(0x0500) & 0x04) != 0);  // SLPAK
    b.w(0x0505, 0x80);                 // WUPIE
    CanFrame f;
    f.id = 0x100;
    b.bus.send(b.tester, f, b.soc.m.now());
    CHECK((b.r(0x0500) & 0x04) == 0);
    CHECK((b.r(0x0504) & 0x80) != 0);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kMscanWakeup)) != 0);
    b.w(0x0504, 0x80);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kMscanWakeup)) == 0);
}

TEST(mscan_id_encoding_round_trips) {
    for (const bool ext : {false, true}) {
        CanFrame f;
        f.isExtended = ext;
        f.id = ext ? 0x1ABCDEF5 : 0x5A5;
        f.isRTR = true;
        const auto idr = Mscan::encode_id(f);
        CanFrame g;
        Mscan::decode_id(idr.data(), g);
        CHECK_EQ(g.id, f.id);
        CHECK(g.isExtended == ext);
        CHECK(g.isRTR);
    }
}

TEST(mscan_takes_classic_frames_only) {
    // The bus between simulated nodes carries the tree's frame type, which can be CAN FD; to a classic controller an
    // FD frame is a form error, never data in a buffer.
    Bench b;
    CanFrame fd;
    fd.id = 0x123;
    fd.isFD = true;
    fd.len = 12;
    CHECK(!b.bus.send(b.tester, fd, b.soc.m.now()));
    CHECK_EQ(b.r(0x0504) & 1, 0);

    CanFrame classic;
    classic.id = 0x123;
    classic.len = 2;
    classic.data[0] = 0x5A;
    CHECK(b.bus.send(b.tester, classic, b.soc.m.now()));
    CHECK_EQ(b.r(0x0504) & 1, 1);
    CHECK_EQ(b.r(0x0544), 0x5A);
    CHECK_EQ(b.r(0x054C), 2);  // RXDLR
}

TEST(mscan_dlc_past_eight_sends_eight_bytes) {
    // A DLC of 9..15 is legal on classic CAN and still means eight bytes.
    Bench b;
    b.w(0x0550, 0x504 >> 3);
    b.w(0x0551, (0x504 & 7) << 5);
    b.w(0x055C, 0x0F);
    b.w(0x0506, 0x01);  // send TX0
    b.run_us(500);
    CHECK_EQ(b.tester.got.size(), 1);
    if (!b.tester.got.empty()) {
        CHECK_EQ(b.tester.got[0].first.len, 8);
    }
}
