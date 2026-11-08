// The read-only session against a fake unit that speaks the protocol from its side: discovery, connect with its CRC,
// reads in flow-controlled blocks of eight frames. Then the unit misbehaving -- a stalled block, frames out of order,
// a refused read, more data than asked, stray traffic on the bus -- and the tool's own refusals.
#include <deque>
#include <functional>
#include <optional>

#include "mctest/test.h"
#include "pdm32/config/crc16.h"
#include "pdm_flash_dump/dump.h"
#include "pdm_flash_dump/session.h"

using namespace pdm_flash_dump;

namespace {

helpers::CanFrame make(uint32_t id, std::initializer_list<uint8_t> bytes, bool extended = false) {
    helpers::CanFrame f;
    f.id = id;
    f.isExtended = extended;
    f.len = static_cast<uint8_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), f.data.begin());
    return f;
}

// A unit with memory, serial 1717, beaconing on its default nodes. Replies are queued as the host's frames arrive,
// which is how the real unit's comms behave at this level.
class FakeUnit final : public Link {
   public:
    std::array<uint8_t, 0x10000> memory{};
    uint16_t serial = 1717;
    uint8_t rx = 0x3F;
    uint8_t tx = 0x3E;

    // Faults.
    int stall_reads = 0;            // this many reads never send their first block
    bool swap_frames = false;       // frames 0 and 1 of a read arrive in the wrong order
    uint8_t read_status = 0;        // the final response's status
    int extra_bytes = 0;            // more data than asked
    bool chatter = false;           // unrelated traffic between every frame
    bool refuse_connect = false;

    std::vector<helpers::CanFrame> heard;

    void beacon() {
        out_.push_back(make(0x0EF00000, {0x03, 0x11, uint8_t(serial >> 8), uint8_t(serial), rx, tx, 0}, true));
    }

    void send(const helpers::CanFrame& frame) override {
        heard.push_back(frame);
        if (frame.isExtended || (frame.id >> 4) != rx) {
            return;
        }
        switch (frame.id & 0x0F) {
            case 9:
                on_connect(frame);
                break;
            case 14:
                on_read(frame);
                break;
            case 8:
                on_flow();
                break;
            default:
                break;
        }
    }

    std::optional<helpers::CanFrame> receive(Clock::time_point) override {
        if (out_.empty()) {
            return std::nullopt;  // nothing more is coming: a stall
        }
        helpers::CanFrame f = out_.front();
        out_.pop_front();
        return f;
    }

   private:
    void push(const helpers::CanFrame& f) {
        if (chatter) {
            out_.push_back(make(0x100, {1, 2, 3}));
            out_.push_back(make(0x18FF0000, {9}, true));
        }
        out_.push_back(f);
    }

    void on_connect(const helpers::CanFrame& frame) {
        const auto body = frame.data_span();
        const uint16_t crc = pdm::config::crc16_ccitt(body.first(6));
        const bool ok = !refuse_connect && body.size() == 8 && body[2] == (serial >> 8) && body[3] == uint8_t(serial) &&
                        body[6] == (crc >> 8) && body[7] == uint8_t(crc);
        push(make((uint32_t{tx} << 4) | 0x0F, {9, 0, uint8_t(ok ? 0 : 1)}));
        if (ok) {
            rx = body[4];
            tx = body[5];
        }
    }

    void on_read(const helpers::CanFrame& frame) {
        const auto b = frame.data_span();
        address_ = static_cast<uint16_t>((b[2] << 8) | b[3]);
        remaining_ = static_cast<size_t>((b[4] << 8) | b[5]) + static_cast<size_t>(extra_bytes);
        seq_ = 0;
        if (stall_reads > 0) {
            --stall_reads;
            remaining_ = 0;
            return;
        }
        send_block();
    }

    void on_flow() {
        if (remaining_ > 0) {
            send_block();
        } else {
            push(make((uint32_t{tx} << 4) | 0x0F, {14, 0, read_status}));
        }
    }

    // Up to eight frames, then wait for flow control -- also after the last data, as the unit does.
    void send_block() {
        std::vector<helpers::CanFrame> block;
        for (int k = 0; k < 8 && remaining_ > 0; ++k) {
            helpers::CanFrame f;
            f.id = (uint32_t{tx} << 4) | seq_;
            const size_t n = std::min<size_t>(8, remaining_);
            for (size_t i = 0; i < n; ++i) {
                f.data[i] = memory[address_++];
            }
            f.len = static_cast<uint8_t>(n);
            remaining_ -= n;
            seq_ = (seq_ + 1) & 7;
            block.push_back(f);
        }
        if (swap_frames && block.size() >= 2) {
            std::swap(block[0], block[1]);
        }
        for (const auto& f : block) {
            push(f);
        }
        if (block.size() < 8) {
            // A short last block: the unit answers at once.
            push(make((uint32_t{tx} << 4) | 0x0F, {14, 0, read_status}));
        }
    }

    std::deque<helpers::CanFrame> out_;
    uint16_t address_ = 0;
    size_t remaining_ = 0;
    uint32_t seq_ = 0;
};

FakeUnit unit_with_pattern() {
    FakeUnit unit;
    for (size_t a = 0; a < unit.memory.size(); ++a) {
        unit.memory[a] = static_cast<uint8_t>(a * 7 + (a >> 8));
    }
    return unit;
}

bool throws(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const ProtocolError&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(session_discovers_connects_and_reads) {
    FakeUnit unit = unit_with_pattern();
    unit.beacon();
    Session session(unit, std::nullopt, std::chrono::milliseconds(50));
    CHECK_EQ(session.discover(std::chrono::milliseconds(50)), 1717);
    session.connect();
    CHECK_EQ(unit.rx, 0x3E);
    for (const uint16_t length : {uint16_t{3}, uint16_t{64}, uint16_t{65}, uint16_t{0x800}}) {
        const auto bytes = session.read(0x8000, length);
        CHECK_EQ(bytes.size(), length);
        mctest::check(std::equal(bytes.begin(), bytes.end(), unit.memory.begin() + 0x8000),
                      "read " + std::to_string(length) + " bytes match the unit's memory");
    }
    session.close_session();
}

TEST(session_sends_only_read_only_requests) {
    FakeUnit unit = unit_with_pattern();
    Session session(unit, 1717, std::chrono::milliseconds(50));
    session.connect();
    (void)session.read(0x1E20, 0x200);
    session.close_session();
    for (const auto& frame : unit.heard) {
        const uint32_t type = frame.id & 0x0F;
        mctest::check(type == 8 || type == 9 || type == 14, "only connect, read and flow control go out");
        CHECK(!frame.isExtended);
        CHECK(frame.len <= 8);
    }
}

TEST(session_refuses_unsafe_reads_without_sending) {
    FakeUnit unit = unit_with_pattern();
    Session session(unit, 1717, std::chrono::milliseconds(50));
    session.connect();
    const size_t before = unit.heard.size();
    CHECK(throws([&] { (void)session.read(0x0000, 16); }));  // the register window
    CHECK(throws([&] { (void)session.read(0x0440, 16); }));  // between RAM-1 and the factory block
    CHECK(throws([&] { (void)session.read(0x7FF0, 32); }));  // crosses from one flash array into the other
    CHECK(throws([&] { (void)session.read(0xFE00, 16); }));  // the SIM registers
    CHECK(throws([&] { (void)session.read(0x8000, 2); }));   // the unit sends no data for under 3 bytes
    CHECK_EQ(unit.heard.size(), before);
}

TEST(session_connect_needs_a_serial_and_sane_nodes) {
    FakeUnit unit = unit_with_pattern();
    Session no_serial(unit, std::nullopt, std::chrono::milliseconds(20));
    CHECK(throws([&] { no_serial.connect(); }));
    CHECK(throws([&] { (void)no_serial.discover(std::chrono::milliseconds(20)); }));  // no beacon
    Session session(unit, 1717, std::chrono::milliseconds(20));
    CHECK(throws([&] { session.connect(0x3F, 0x3E); }));  // '?' is the pre-session node
    CHECK(throws([&] { session.connect(0x80, 0x3F); }));  // would not fit an 11-bit id
    CHECK(unit.heard.empty());

    unit.refuse_connect = true;
    CHECK(throws([&] { session.connect(); }));
}

TEST(session_retries_a_stalled_read_then_gives_up) {
    FakeUnit unit = unit_with_pattern();
    Session session(unit, 1717, std::chrono::milliseconds(10));
    session.connect();
    unit.stall_reads = 2;
    const auto bytes = session.read(0x8000, 100);
    CHECK_EQ(bytes.size(), 100);
    CHECK_EQ(session.retries(), 2);

    unit.stall_reads = 10;
    CHECK(throws([&] { (void)session.read(0x8000, 100, 0, 3); }));
}

TEST(session_rejects_a_misbehaving_unit) {
    {
        FakeUnit unit = unit_with_pattern();
        Session session(unit, 1717, std::chrono::milliseconds(10));
        session.connect();
        unit.swap_frames = true;
        CHECK(throws([&] { (void)session.read(0x8000, 100); }));  // out of order
    }
    {
        FakeUnit unit = unit_with_pattern();
        Session session(unit, 1717, std::chrono::milliseconds(10));
        session.connect();
        unit.read_status = 9;
        CHECK(throws([&] { (void)session.read(0x8000, 100); }));  // region locked
    }
    {
        FakeUnit unit = unit_with_pattern();
        Session session(unit, 1717, std::chrono::milliseconds(10));
        session.connect();
        unit.extra_bytes = 5;
        CHECK(throws([&] { (void)session.read(0x8000, 100); }));  // more than asked
    }
}

TEST(session_ignores_other_traffic) {
    FakeUnit unit = unit_with_pattern();
    unit.chatter = true;
    unit.beacon();
    Session session(unit, std::nullopt, std::chrono::milliseconds(50));
    CHECK_EQ(session.discover(std::chrono::milliseconds(50)), 1717);
    session.connect();
    const auto bytes = session.read(0x8000, 200);
    CHECK_EQ(bytes.size(), 200);
    CHECK(std::equal(bytes.begin(), bytes.end(), unit.memory.begin() + 0x8000));
}

TEST(dump_blocks_and_srec) {
    Image image;
    // A format-1 block of 10 bytes at the calibration address with a good CRC, and a config whose CRC is wrong.
    std::vector<uint8_t> cal{0, 1, 0, 10, 1, 2, 3, 4};
    const uint16_t crc = pdm::config::crc16_ccitt(cal);
    cal.push_back(uint8_t(crc >> 8));
    cal.push_back(uint8_t(crc));
    for (uint32_t k = 0; k < cal.size(); ++k) {
        image[0x7B00 + k] = cal[k];
    }
    const BlockCheck good = check_block(image, 0x7B00, 1);
    CHECK(good.present && good.header_ok && good.crc_ok && good.bytes == cal);

    image[0x7B05] ^= 0xFF;
    CHECK(!check_block(image, 0x7B00, 1).crc_ok);
    CHECK(!check_block(image, 0x7B00, 9).header_ok);  // wrong format
    image.erase(0x7B08);
    CHECK(!check_block(image, 0x7B00, 1).header_ok);  // a byte missing
    CHECK(!check_block(image, 0x1E20, 9).present);

    Image small{{0x8000, 0xB0}, {0x8001, 0x00}, {0x9000, 0x42}};
    const std::string srec = to_srec(small, "t");
    CHECK(srec.find("S1058000B000") != std::string::npos);
    CHECK(srec.find("S104900042") != std::string::npos);
    CHECK(srec.find("S9030000FC") != std::string::npos);
}
