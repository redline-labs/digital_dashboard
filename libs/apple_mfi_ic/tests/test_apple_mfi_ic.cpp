// Drives AppleMFIIC against a fake coprocessor that reproduces the timing
// measured on the real part (apple_mfi_ic.cpp, top of file):
//
//   * asleep after 30 ms idle: the first START is NACKed and wakes it
//   * busy for 1 ms after a successful register-select write: everything NACKs
//   * a NACKed write does NOT set the register pointer; a NACKed read leaves it
//   * reads auto-increment to the next register
//
// and, optionally, a transport as slow as the MCP2221A bridge, where every
// transaction takes milliseconds and a NACK costs tens of them. The driver's
// retry logic is transport-agnostic, so the same test runs both shapes and
// bounds the time each takes: a retry policy tuned for one transport must not
// stall on the other.
//
// And each shape again on a LOADED HOST: the fake treats any gap between two
// transactions longer than a back-to-back pair as the driver having slept,
// and stalls 40 ms before answering -- the sleep came back late, past the
// part's idle threshold, so the part is asleep again. That is what a busy
// build machine did to this test, and what a busy car can do to the driver.
// Real sleeps only ever run long, so machine load can make the scenario
// worse, never kinder: the driver must not depend on a sleep ending on time.
// Two harsher shapes: a host frozen for longer than an operation's whole
// budget, and one that also preempts mid-sequence while the part wakes by a
// stretched ACK with its register pointer lost -- where trusting a read that
// may have met a sleeping part returns garbage.
#include "apple_mfi_ic/apple_mfi_ic.h"
#include "i2c_bus/i2c_bus.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

#define CHECK(cond)                                                                       \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

struct Transport
{
    const char* name;
    std::chrono::microseconds latency;   // cost of every transaction
    std::chrono::microseconds nack_cost; // extra cost of a NACKed one
    // Loaded host: every time the driver sleeps, it gets the CPU back this
    // late -- past the part's idle threshold, so the part is asleep again.
    std::chrono::milliseconds late_wakeup{0};
    // Frozen host: the FIRST late wake-up is this long instead -- longer than
    // an operation's whole time budget.
    std::chrono::milliseconds frozen_once{0};
    // The other way the part was seen to wake: ACK the first START after a
    // ~12 ms clock stretch instead of NACKing it. What sleep does to the
    // register pointer was never measured; this assumes the worst -- lost --
    // so a driver that trusts a read that may have met a sleeping part reads
    // garbage.
    bool wakes_by_stretching = false;
    // Involuntary preemption: the host also takes the CPU away mid-sequence,
    // stalling before every Nth transaction wherever it falls -- between a
    // register select and its read, say -- not only when the driver sleeps.
    int preempt_every = 0;
};

constexpr Transport kNative{"native controller", std::chrono::microseconds(200),
                            std::chrono::microseconds(0)};
// hidapi path: ~3 ms per HID round trip; a NACK leaves the engine latched and
// the backend polls it back to idle (status, cancel, 2 ms, status), ~12 ms.
// That has to stay under the part's ~30 ms idle-to-sleep threshold or no
// retry policy can help: the recovery itself would put the part back to sleep
// before the next START. The fake enforces that: raise nack_cost past 30 ms
// and wake() fails, which is the right answer, not a driver bug.
constexpr Transport kBridge{"MCP2221A bridge", std::chrono::microseconds(3000),
                            std::chrono::microseconds(12000)};
constexpr auto kLateWakeup = std::chrono::milliseconds(40);
constexpr Transport kNativeLoaded{"native, loaded", kNative.latency, kNative.nack_cost, kLateWakeup};
constexpr Transport kBridgeLoaded{"bridge, loaded", kBridge.latency, kBridge.nack_cost, kLateWakeup};
constexpr Transport kNativeFrozen{"native, frozen", kNative.latency, kNative.nack_cost, kLateWakeup,
                                  std::chrono::milliseconds(1500)};
constexpr Transport kNativeStretch{"native, stretch", kNative.latency, kNative.nack_cost, kLateWakeup,
                                   std::chrono::milliseconds(0), true, 11};

// Longer than any back-to-back pair of transactions takes to issue, shorter
// than the driver's shortest sleep: a gap past it means the driver yielded.
constexpr auto kYieldGap = std::chrono::microseconds(300);

class FakeCoprocessor : public i2c::Bus
{
  public:
    explicit FakeCoprocessor(Transport transport) : transport_(transport)
    {
        // Each register is its own object with its own length, as on the part:
        // 0x11 is a 2-byte length and 0x12 the 128-byte response it describes.
        regs_[0x00] = {0x05};  // device version
        regs_[0x01] = {0x01};  // authentication revision
        regs_[0x02] = {0x02};  // protocol major
        regs_[0x03] = {0x00};  // protocol minor
        regs_[0x10] = {0x00};  // control/status
        regs_[0x11] = {0x00, 0x80};
        std::vector<uint8_t> response(128);
        for (size_t i = 0; i < response.size(); ++i)
        {
            response[i] = static_cast<uint8_t>(i);
        }
        regs_[0x12] = response;
    }

    bool open() override { return true; }
    void close() override {}
    bool is_open() const override { return true; }
    std::string description() const override { return transport_.name; }
    bool probe(uint8_t address) override { return !read(address, 1).empty(); }

    bool write(uint8_t address, const std::vector<uint8_t>& data) override
    {
        ++writes;
        if (!start(address))
        {
            // Measured: a NACKed register select leaves nothing selected. A
            // NACKed READ (busy window) does not disturb the pointer.
            pointer_valid_ = false;
            return false;
        }
        if (!data.empty())
        {
            pointer_ = data[0];
            pointer_valid_ = true;
            if (data.size() > 1)
            {
                regs_[pointer_] = std::vector<uint8_t>(data.begin() + 1, data.end());
            }
            if (data.size() > 1 && data[0] == 0x10 && data[1] == 0x01)
            {
                // Start authentication: status 0x01 for a while, then 0x10.
                auth_done_at_ = Clock::now() + std::chrono::milliseconds(50);
                regs_[0x10] = {0x01};
            }
        }
        busy_until_ = Clock::now() + std::chrono::microseconds(1000);
        return true;
    }

    std::vector<uint8_t> read(uint8_t address, size_t length) override
    {
        ++reads;
        if (!start(address))
        {
            return {};
        }
        if (auth_done_at_ && Clock::now() >= *auth_done_at_)
        {
            regs_[0x10] = {0x10};
        }
        // Nothing selected since the last NACKed write: the part returns
        // whatever it has, which on hardware looked like 0xff/0x00.
        std::vector<uint8_t> out(length, 0xff);
        if (pointer_valid_)
        {
            const auto it = regs_.find(pointer_);
            if (it != regs_.end())
            {
                for (size_t i = 0; i < length && i < it->second.size(); ++i)
                {
                    out[i] = it->second[i];
                }
            }
        }
        ++pointer_;  // the part auto-increments to the next register
        return out;
    }

    int writes = 0;
    int reads = 0;
    int nacks = 0;
    int stalls = 0;     // late wake-ups the loaded host imposed
    int stretches = 0;  // wakes answered with a stretched ACK
    int transactions_ = 0;

  private:
    // The START of any transaction. False = NACK.
    bool start(uint8_t address)
    {
        ++transactions_;
        const bool preempted = transport_.preempt_every > 0 && transactions_ % transport_.preempt_every == 0;
        if (transport_.late_wakeup.count() > 0 && (preempted || Clock::now() - last_end_ > kYieldGap))
        {
            const bool freeze = stalls == 0 && transport_.frozen_once.count() > 0;
            ++stalls;
            std::this_thread::sleep_for(freeze ? transport_.frozen_once : transport_.late_wakeup);
        }
        std::this_thread::sleep_for(transport_.latency);
        const auto now = Clock::now();
        bool ack = address == AppleMFIIC::I2C_ADDRESS;
        if (ack && now - last_activity_ > std::chrono::milliseconds(30))
        {
            if (transport_.wakes_by_stretching)
            {
                // Asleep: this START is held ~12 ms, then ACKed, with the
                // register pointer gone.
                ++stretches;
                pointer_valid_ = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
            else
            {
                ack = false;  // asleep: this START wakes it and is NACKed
            }
        }
        if (ack && now < busy_until_)
        {
            ack = false;  // still busy after the last register select
        }
        last_activity_ = now;
        if (!ack)
        {
            ++nacks;
            std::this_thread::sleep_for(transport_.nack_cost);
        }
        last_end_ = Clock::now();
        return ack;
    }

    Transport transport_;
    std::map<uint8_t, std::vector<uint8_t>> regs_;
    uint8_t pointer_ = 0;
    bool pointer_valid_ = false;
    Clock::time_point last_activity_ = Clock::now() - std::chrono::seconds(1);
    Clock::time_point busy_until_ = Clock::now();
    Clock::time_point last_end_ = Clock::now();  // when the last transaction returned to the driver
    std::optional<Clock::time_point> auth_done_at_;
};

void exercise(Transport transport)
{
    auto fake = std::make_unique<FakeCoprocessor>(transport);
    FakeCoprocessor* raw = fake.get();
    AppleMFIIC ic(std::move(fake));

    const auto t0 = Clock::now();
    CHECK(ic.init());

    // Cold: the part is asleep, so the first START is the wake NACK.
    auto info = ic.query_device_info();
    CHECK(info.has_value());
    CHECK(info->device_version == 0x05);
    CHECK(info->authentication_revision == 0x01);
    CHECK(info->authentication_protocol_major_version == 2);
    CHECK(info->authentication_protocol_minor_version == 0);

    // Let it fall asleep again between two queries and re-read.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    info = ic.query_device_info();
    CHECK(info.has_value());
    CHECK(info->device_version == 0x05);
    CHECK(info->authentication_protocol_major_version == 2);

    // A full challenge: writes, the auth kick, status polling, then a
    // 128-byte response read through the same pointer/busy rules.
    const std::vector<uint8_t> challenge(20, 0xa5);
    const auto signature = ic.sign_challenge(challenge);
    CHECK(signature.has_value());
    CHECK(signature->size() == 128);
    for (size_t i = 0; i < signature->size(); ++i)
    {
        CHECK((*signature)[i] == static_cast<uint8_t>(i));
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    std::printf("%-18s  %4lld ms   writes %3d  reads %3d  nacks %3d  late wake-ups %3d\n", transport.name,
                static_cast<long long>(elapsed.count()), raw->writes, raw->reads, raw->nacks, raw->stalls);

    CHECK(raw->nacks > 0);  // the model actually exercised the retry paths
    CHECK(!transport.wakes_by_stretching || raw->stretches > 0);
    CHECK(transport.late_wakeup.count() == 0 || raw->stalls > 0);  // and the loaded host actually stalled it
    // A policy that spins on a part that will not answer, or redoes work it
    // already had, shows as transactions -- which, unlike time, a loaded
    // machine does not inflate. An efficient exchange takes 30-95.
    // Time is not asserted at all: on a loaded machine it measures the
    // machine. A hang is ctest's timeout to catch.
    CHECK(raw->writes + raw->reads < 200);
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    exercise(kNative);
    exercise(kBridge);
    exercise(kNativeLoaded);
    exercise(kBridgeLoaded);
    exercise(kNativeFrozen);
    exercise(kNativeStretch);
    std::puts("ok");
    return 0;
}
