// The MPC5554 from a bare image assembled here: the boot ROM's reset configuration and TLB entries, eSCI A as a
// console, FlexCAN transmit at the programmed bit rate and receive through the masks, the INTC in software vector
// mode, and the stubs.
#include <string>
#include <vector>

#include "e200/assembler.h"
#include "mctest/test.h"
#include "mpc55xx/soc.h"

using namespace sim;
using namespace sim::mpc55xx;
using e200::Asm;

namespace {

class Listener final : public sys::CanNode {
   public:
    std::vector<sys::CanFrame> frames;
    std::vector<uint64_t> at;
    bool receive(const sys::CanFrame& frame, const sys::CanNode&, uint64_t now) override {
        frames.push_back(frame);
        at.push_back(now);
        return true;
    }
};

// 500 kbit/s from the 125 MHz system clock: 10 * (1 + 8 + 8 + 8) clocks per bit
constexpr uint32_t kCtrl500k = (9u << 24) | (7u << 19) | (7u << 16) | 0x2000 | 7;

// The image: the reset configuration, the vector, then `body` at 0x100.
std::vector<uint8_t> image(const Asm& body) {
    std::vector<uint8_t> out(0x100, 0);
    out[0] = 0x00;
    out[1] = 0x5A;  // boot identifier
    out[4] = 0x00;
    out[5] = 0x00;
    out[6] = 0x01;
    out[7] = 0x00;  // reset vector 0x100
    const auto code = body.bytes();
    out.insert(out.end(), code.begin(), code.end());
    return out;
}

// r10 = FlexCAN A, running at 500 kbit/s with buffer 0 receiving ID 0x100
void can_init(Asm& a) {
    a.li32(10, map::kFlexCanA);
    a.li32(3, kCtrl500k);
    a.stw(3, 0x04, 10);
    a.li32(3, 0x100 << 18);
    a.stw(3, 0x80 + 4, 10);
    a.li32(3, 0x04000000);  // buffer 0: receive, empty
    a.stw(3, 0x80, 10);
    a.li(3, 0x3F);
    a.stw(3, 0x00, 10);  // MCR: leave disable and freeze, 64 buffers
}

}  // namespace

TEST(mpc55xx_bam_boots_from_the_reset_configuration) {
    e200::Machine m;
    Mpc5554 soc(m);
    std::string console;
    soc.esci_a->on_byte = [&](uint8_t b) { console += char(b); };
    Asm a(0x100);
    auto spin = a.label();
    a.li32(9, map::kEsciA);
    for (char c : std::string("boot\n")) {
        a.li(3, int16_t(c));
        a.sth(3, 6, 9);
    }
    a.bind(spin);
    a.b(spin);
    CHECK(soc.load_flash(image(a)));
    CHECK(soc.reset());
    CHECK_EQ(m.state.pc, 0x100u);
    m.run_until(1000);
    CHECK(console == "boot\n");
}

TEST(mpc55xx_no_reset_configuration_does_not_boot) {
    e200::Machine m;
    Mpc5554 soc(m);
    CHECK(!soc.reset());  // erased flash
    CHECK_EQ(m.state.pc, 0xFFFFFFFCu);
}

TEST(mpc55xx_flexcan_transmits_at_the_bit_rate) {
    e200::Machine m;
    Mpc5554 soc(m);
    Listener bus;
    soc.can_bus[0].attach(bus);
    Asm a(0x100);
    auto spin = a.label();
    can_init(a);
    a.li32(3, 0x123 << 18);
    a.stw(3, 0x80 + 16 * 8 + 4, 10);
    a.li32(3, 0x11223344);
    a.stw(3, 0x80 + 16 * 8 + 8, 10);
    a.li32(3, 0x55667788);
    a.stw(3, 0x80 + 16 * 8 + 12, 10);
    a.li32(3, 0x0C080000);  // transmit, 8 bytes
    a.stw(3, 0x80 + 16 * 8, 10);
    a.bind(spin);
    a.b(spin);
    CHECK(soc.load_flash(image(a)));
    CHECK(soc.reset());
    m.run_until(125'000);  // 1 ms
    CHECK_EQ(soc.can[0]->bit_rate(), 500'000u);
    CHECK_EQ(bus.frames.size(), size_t(1));
    if (!bus.frames.empty()) {
        CHECK_EQ(bus.frames[0].id, 0x123u);
        CHECK_EQ(bus.frames[0].len, 8);
        CHECK_EQ(bus.frames[0].data[0], 0x11);
        CHECK_EQ(bus.frames[0].data[7], 0x88);
        // 111 bits at 500 kbit/s = 222 us after the transmit request (about cycle 20)
        CHECK(bus.at[0] > 222 * 125 && bus.at[0] < 222 * 125 + 200);
    }
    uint32_t iflag = 0;
    CHECK(m.peek(map::kFlexCanA + 0x30, 4, &iflag));
    CHECK_EQ(iflag, 1u << 8);
}

TEST(mpc55xx_flexcan_receives_through_the_mask_and_echoes_by_interrupt) {
    e200::Machine m;
    Mpc5554 soc(m);
    Listener bus;
    soc.can_bus[0].attach(bus);

    // the interrupt handler: acknowledge the INTC, copy buffer 0 to buffer 9 with ID + 1, transmit, release
    Asm h(0x400);
    h.li32(20, map::kIntc);
    h.lwz(21, 0x10, 20);       // IACKR
    h.lwz(3, 0x80 + 4, 10);    // the ID
    h.addis(3, 3, 1 << 2);     // + 1 in the standard ID field (bit 18)
    h.stw(3, 0x80 + 16 * 9 + 4, 10);
    h.lwz(3, 0x80 + 8, 10);
    h.stw(3, 0x80 + 16 * 9 + 8, 10);
    h.lwz(3, 0x80 + 12, 10);
    h.stw(3, 0x80 + 16 * 9 + 12, 10);
    h.lwz(3, 0x80, 10);
    h.rlwinm(3, 3, 0, 12, 15);  // the length
    h.oris(3, 3, 0x0C00);
    h.stw(3, 0x80 + 16 * 9, 10);
    h.li32(3, 0x04000000);
    h.stw(3, 0x80, 10);         // buffer 0 empty again
    h.li(3, 1);
    h.stw(3, 0x30, 10);         // clear its flag
    h.li(3, 0);
    h.stw(3, 0x18, 20);         // EOIR
    h.rfi();

    Asm a(0x100);
    auto idle = a.label();
    can_init(a);
    a.li(3, 1);
    a.stw(3, 0x28, 10);         // IMRL: buffer 0
    a.li32(20, map::kIntc);
    a.li(3, 5);
    a.stb(3, 0x40 + kFlexCanASources.buffer0, 20);  // its priority
    a.li(3, 0);
    a.stw(3, 0x08, 20);         // CPR 0
    a.li32(3, 0x400);
    a.mtspr(e200::spr::kIvor0 + 4, 3);
    a.wrteei(1);
    a.bind(idle);
    a.wait();
    a.b(idle);

    std::vector<uint8_t> img = image(a);
    img.resize(0x400, 0);
    const auto hb = h.bytes();
    img.insert(img.end(), hb.begin(), hb.end());
    CHECK(soc.load_flash(img));
    CHECK(soc.reset());
    m.run_until(10'000);

    sys::CanFrame in{};
    in.id = 0x100;
    in.len = 3;
    in.data[0] = 0xAA;
    in.data[1] = 0xBB;
    in.data[2] = 0xCC;
    CHECK(soc.can_bus[0].send(bus, in, m.now()));
    sys::CanFrame other{};
    other.id = 0x200;  // not matched by buffer 0's ID under the global mask
    soc.can_bus[0].send(bus, other, m.now());
    m.run_until(200'000);

    CHECK_EQ(soc.can[0]->frames_received(), uint64_t(1));
    CHECK_EQ(bus.frames.size(), size_t(1));
    if (!bus.frames.empty()) {
        CHECK_EQ(bus.frames[0].id, 0x101u);
        CHECK_EQ(bus.frames[0].len, 3);
        CHECK_EQ(bus.frames[0].data[2], 0xCC);
    }
    CHECK_EQ(soc.intc->current_priority(), 0u);  // EOIR restored it
    CHECK(m.waiting());
}
