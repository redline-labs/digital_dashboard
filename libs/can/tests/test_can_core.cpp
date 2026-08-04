// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parts of the CAN stack that can be wrong without any hardware being
// present, which is most of the parts that matter.
//
// Bit timing is the big one. A controller whose bit rate is a fraction of a per
// cent off, or whose sample point sits in a different place from everyone
// else's, produces a bus that works on a short bench cable and fails in a
// vehicle. There is no way to see that on an oscilloscope after the fact and no
// way to see it in a code review, so it is checked here against the rates and
// clocks the hardware actually uses.

#include "can/backend.h"
#include "can/bitrate.h"
#include "can/channel_id.h"
#include "can/dlc.h"
#include "can/virtual_backend.h"

#include <spdlog/spdlog.h>

#include <string>
#include <thread>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

// PCAN-USB FD family: an 80 MHz controller clock.
const can::BitTimingLimits kPcanFdNominal {
    .clockHz = 80000000,
    .tseg1Min = 1,
    .tseg1Max = 64,
    .tseg2Min = 1,
    .tseg2Max = 16,
    .sjwMax = 16,
    .brpMin = 1,
    .brpMax = 1024,
};

const can::BitTimingLimits kPcanFdData {
    .clockHz = 80000000,
    .tseg1Min = 1,
    .tseg1Max = 16,
    .tseg2Min = 1,
    .tseg2Max = 8,
    .sjwMax = 4,
    .brpMin = 1,
    .brpMax = 1024,
};

// ============================================================================
// Bit timing
// ============================================================================

void test_standard_rates_are_exact()
{
    // Every rate a vehicle bus is likely to use, at the PCAN FD clock. All of
    // these divide 80 MHz exactly, so anything other than an exact answer is a
    // bug in the solver rather than a limitation of the hardware.
    const uint32_t rates[] = { 1000000, 800000, 500000, 250000, 125000, 100000, 50000, 20000 };

    for (uint32_t rate : rates)
    {
        auto timing = can::solve_bit_timing(rate, 0, kPcanFdNominal);
        check(timing.has_value(), fmt::format("{} bit/s has a solution", rate));
        if (!timing.has_value())
        {
            SPDLOG_ERROR("  {}", can::to_string(timing.error()));
            continue;
        }

        check(timing->bitrateBps == rate,
              fmt::format("{} bit/s comes out exact, not {}", rate, timing->bitrateBps));

        // clock == brp * tq * bitrate is the identity the whole calculation
        // rests on; check it directly rather than trusting the reported rate.
        const uint64_t reconstructed
            = static_cast<uint64_t>(timing->brp) * timing->tq() * timing->bitrateBps;
        check(reconstructed == kPcanFdNominal.clockHz,
              fmt::format("{} bit/s: brp*tq*bitrate reconstructs the 80 MHz clock", rate));

        // Segments inside what the controller accepts.
        check(timing->tseg1 >= kPcanFdNominal.tseg1Min && timing->tseg1 <= kPcanFdNominal.tseg1Max,
              fmt::format("{} bit/s: tseg1 {} is within limits", rate, timing->tseg1));
        check(timing->tseg2 >= kPcanFdNominal.tseg2Min && timing->tseg2 <= kPcanFdNominal.tseg2Max,
              fmt::format("{} bit/s: tseg2 {} is within limits", rate, timing->tseg2));
        check(timing->sjw >= 1 && timing->sjw <= kPcanFdNominal.sjwMax
                  && timing->sjw <= timing->tseg2,
              fmt::format("{} bit/s: sjw {} is legal and no wider than tseg2", rate, timing->sjw));
    }
}

void test_sample_point_lands_where_asked()
{
    // The CiA defaults, which is where every other node on the bus will be.
    check(can::default_sample_point_permille(125000) == 875, "125 kbit/s samples at 87.5%");
    check(can::default_sample_point_permille(500000) == 875, "500 kbit/s samples at 87.5%");
    check(can::default_sample_point_permille(800000) == 800, "800 kbit/s samples at 80%");
    check(can::default_sample_point_permille(1000000) == 750, "1 Mbit/s samples at 75%");

    auto timing = can::solve_bit_timing(500000, 875, kPcanFdNominal);
    check(timing.has_value(), "500 kbit/s at 87.5% solves");
    if (timing.has_value())
    {
        // Exact, because 87.5% of a 16-quantum bit is a whole number of quanta.
        check(timing->samplePointPermille == 875,
              fmt::format("and lands exactly on 87.5%, not {}.{}%",
                          timing->samplePointPermille / 10, timing->samplePointPermille % 10));
    }

    // A request that cannot be hit exactly should still come close rather than
    // failing or silently taking the default.
    auto odd = can::solve_bit_timing(500000, 700, kPcanFdNominal);
    check(odd.has_value(), "an unusual sample point still solves");
    if (odd.has_value())
    {
        const int drift = static_cast<int>(odd->samplePointPermille) - 700;
        check(drift > -30 && drift < 30,
              fmt::format("and lands within 3% of it (got {}.{}%)", odd->samplePointPermille / 10,
                          odd->samplePointPermille % 10));
        check(odd->bitrateBps == 500000, "without giving up the bit rate to get there");
    }
}

void test_fd_data_phase()
{
    // The data phase has a much smaller tseg range, so the same rate needs a
    // different solution -- this is why the solver takes the limits rather
    // than assuming one set.
    const uint32_t rates[] = { 2000000, 4000000, 5000000, 8000000 };

    for (uint32_t rate : rates)
    {
        auto timing = can::solve_bit_timing(rate, 0, kPcanFdData);
        check(timing.has_value(), fmt::format("{} bit/s data phase has a solution", rate));
        if (!timing.has_value())
        {
            SPDLOG_ERROR("  {}", can::to_string(timing.error()));
            continue;
        }
        check(timing->bitrateBps == rate, fmt::format("{} bit/s data phase is exact", rate));
        check(timing->tseg1 <= kPcanFdData.tseg1Max && timing->tseg2 <= kPcanFdData.tseg2Max,
              fmt::format("{} bit/s data phase fits the narrower FD segment limits", rate));
    }
}

void test_impossible_rates_are_refused()
{
    // Faster than the clock can clock.
    auto tooFast = can::solve_bit_timing(90000000, 0, kPcanFdNominal);
    check(!tooFast.has_value(), "a bit rate above the controller clock is refused");
    check(!tooFast.has_value() && tooFast.error().kind == can::Error::Kind::Unsupported,
          "and refused as unsupported rather than as an I/O failure");

    // Slower than the largest prescaler and longest bit can reach.
    auto tooSlow = can::solve_bit_timing(100, 0, kPcanFdNominal);
    check(!tooSlow.has_value(), "a bit rate below what the prescaler can reach is refused");

    auto zero = can::solve_bit_timing(0, 0, kPcanFdNominal);
    check(!zero.has_value() && zero.error().kind == can::Error::Kind::InvalidArgument,
          "zero is an invalid argument, not an unsupported rate");

    // A clock that cannot produce the rate within CiA's 0.5% tolerance must
    // fail rather than return something that nearly works -- "nearly" is how
    // you get a bus that runs for an hour and then does not.
    can::BitTimingLimits awkward = kPcanFdNominal;
    awkward.clockHz = 7300000;
    auto offRate = can::solve_bit_timing(500000, 0, awkward);
    if (offRate.has_value())
    {
        const uint32_t error = offRate->bitrateBps > 500000 ? offRate->bitrateBps - 500000
                                                            : 500000 - offRate->bitrateBps;
        check(error * 1000 / 500000 <= 5,
              "a solution that is returned is always within 0.5% of what was asked for");
    }
}

void test_timing_at_other_clocks()
{
    // Not every adapter is 80 MHz. The classic PCAN-USB is an SJA1000 at
    // 8 MHz, and a solver that had 80 MHz baked in would quietly produce
    // nonsense for it.
    can::BitTimingLimits sja1000 {
        .clockHz = 8000000,
        .tseg1Min = 1,
        .tseg1Max = 16,
        .tseg2Min = 1,
        .tseg2Max = 8,
        .sjwMax = 4,
        .brpMin = 1,
        .brpMax = 64,
    };

    for (uint32_t rate : { 500000u, 250000u, 125000u })
    {
        auto timing = can::solve_bit_timing(rate, 0, sja1000);
        check(timing.has_value() && timing->bitrateBps == rate,
              fmt::format("{} bit/s is exact on an 8 MHz SJA1000 too", rate));
    }
}

// ============================================================================
// CAN FD length coding
// ============================================================================

void test_dlc()
{
    // The nine sizes above eight bytes are not linear, and the table is the
    // whole reason this is a function.
    const uint8_t expected[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    for (uint8_t dlc = 0; dlc < 16; ++dlc)
    {
        check(can::dlc_to_length(dlc, true) == expected[dlc],
              fmt::format("FD DLC {} is {} bytes", dlc, expected[dlc]));
    }

    // Classic CAN permits DLC 9..15 on the wire but they all mean eight.
    check(can::dlc_to_length(9, false) == 8, "classic DLC 9 is eight bytes, not twelve");
    check(can::dlc_to_length(15, false) == 8, "classic DLC 15 is eight bytes");

    // Round trips for the representable sizes.
    for (uint8_t dlc = 0; dlc < 16; ++dlc)
    {
        const uint8_t length = can::dlc_to_length(dlc, true);
        check(can::length_to_dlc(length, true) == dlc,
              fmt::format("{} bytes round-trips back to DLC {}", length, dlc));
        check(can::is_valid_can_length(length, true),
              fmt::format("{} bytes is a representable FD length", length));
    }

    // A length between two sizes has to round up, and the caller has to be
    // able to find out that it will.
    check(!can::is_valid_can_length(10, true), "10 bytes is not a representable FD length");
    check(can::round_up_can_length(10, true) == 12, "and occupies twelve bytes on the wire");
    check(can::round_up_can_length(33, true) == 48, "33 bytes occupies 48");
    check(can::round_up_can_length(64, true) == 64, "64 bytes is exact");
    check(can::round_up_can_length(65, true) == 64, "and nothing larger exists");

    check(can::is_valid_can_length(8, false), "8 bytes is fine for classic CAN");
    check(!can::is_valid_can_length(9, false), "9 is not");
}

// ============================================================================
// Channel naming
// ============================================================================

void test_channel_id_parsing()
{
    struct Case
    {
        const char* text;
        const char* backend;
        const char* device;
        uint8_t channel;
    };

    const Case cases[] = {
        { "socketcan:can0", "socketcan", "can0", 0 },
        { "pcan:0", "pcan", "0", 0 },
        { "pcan:0/1", "pcan", "0", 1 },
        { "pcan:LSN00123/1", "pcan", "LSN00123", 1 },
        { "virtual:a", "virtual", "a", 0 },
        // The backend is case-insensitive; the device is not, because a serial
        // number's case is its own business.
        { "SocketCAN:can0", "socketcan", "can0", 0 },
    };

    for (const auto& c : cases)
    {
        auto id = can::parse_channel_id(c.text);
        check(id.has_value(), fmt::format("'{}' parses", c.text));
        if (!id.has_value())
        {
            SPDLOG_ERROR("  {}", can::to_string(id.error()));
            continue;
        }
        check(id->backend == c.backend, fmt::format("'{}' backend is '{}'", c.text, c.backend));
        check(id->device == c.device, fmt::format("'{}' device is '{}'", c.text, c.device));
        check(id->channel == c.channel, fmt::format("'{}' channel is {}", c.text, c.channel));
    }

    // Round trip. The channel suffix is dropped when it is zero, so the
    // printed form is canonical rather than merely equivalent.
    check(can::parse_channel_id("pcan:0/0")->toString() == "pcan:0",
          "channel 0 is not printed, so 'pcan:0/0' canonicalises to 'pcan:0'");
    check(can::parse_channel_id("pcan:0/2")->toString() == "pcan:0/2",
          "a non-zero channel round-trips");
    check(can::parse_channel_id("socketcan:can0")->toString() == "socketcan:can0",
          "an interface name round-trips");

    // Rejections.
    const char* bad[] = { "can0", "", ":can0", "socketcan:", "pcan:0/300" };
    for (const char* text : bad)
    {
        check(!can::parse_channel_id(text).has_value(),
              fmt::format("'{}' is refused rather than guessed at", text));
    }
}

// ============================================================================
// The loopback bus
// ============================================================================

can::Registry make_virtual_only_registry()
{
    can::Registry registry;
    registry.add(can::make_virtual_backend());
    return registry;
}

helpers::CanFrame make_frame(uint32_t id, std::initializer_list<uint8_t> bytes)
{
    helpers::CanFrame frame {};
    frame.id = id;
    frame.len = static_cast<uint8_t>(bytes.size());
    size_t i = 0;
    for (uint8_t byte : bytes)
    {
        frame.data[i++] = byte;
    }
    return frame;
}

void test_virtual_bus()
{
    auto registry = make_virtual_only_registry();
    can::OpenOptions options;
    options.bitrate.nominalBps = 500000;

    auto a = registry.open("virtual:test", options);
    auto b = registry.open("virtual:test", options);
    check(a.has_value() && b.has_value(), "two channels open on the same virtual bus");
    if (!a.has_value() || !b.has_value())
    {
        return;
    }

    check(can::virtual_bus_channel_count("test") == 2, "the bus knows about both");
    check((*a)->running(), "a channel opened with start comes up running");

    // A frame sent by one arrives at the other, and not back at the sender --
    // a CAN controller without loopback does not hear itself.
    check((*a)->send(make_frame(0x123, { 0xDE, 0xAD })).has_value(), "a sends a frame");

    helpers::CanFrame received[4];
    auto count = (*b)->receive(received, can::Duration { 200 });
    check(count.has_value() && *count == 1, "b receives exactly one frame");
    if (count.has_value() && *count == 1)
    {
        check(received[0].id == 0x123, "with the right identifier");
        check(received[0].len == 2 && received[0].data[0] == 0xDE && received[0].data[1] == 0xAD,
              "and the right payload");
    }

    auto selfCount = (*a)->receive(received, can::Duration { 20 });
    check(selfCount.has_value() && *selfCount == 0, "the sender does not hear its own frame");

    // An injected frame reaches everyone, standing in for a third node.
    can::virtual_bus_inject("test", make_frame(0x456, { 0x01 }));
    auto injectedA = (*a)->receive(received, can::Duration { 200 });
    check(injectedA.has_value() && *injectedA == 1, "an injected frame reaches a");
    auto injectedB = (*b)->receive(received, can::Duration { 200 });
    check(injectedB.has_value() && *injectedB == 1, "and b");

    // Statistics count what went past: one frame out of a, and two into b --
    // a's frame plus the injected one.
    check((*a)->statistics().txFrames == 1, "the sender counted one transmission");
    check((*b)->statistics().rxFrames == 2, "the receiver counted both frames it saw");

    // Timeout on a quiet bus is not an error: zero frames, no failure.
    auto quiet = (*b)->receive(received, can::Duration { 20 });
    check(quiet.has_value() && *quiet == 0, "a quiet bus times out with zero frames, not an error");
}

void test_virtual_buses_are_separate()
{
    auto registry = make_virtual_only_registry();
    auto here = registry.open("virtual:here", can::OpenOptions {});
    auto there = registry.open("virtual:there", can::OpenOptions {});
    if (!here.has_value() || !there.has_value())
    {
        check(false, "both buses open");
        return;
    }

    check((*here)->send(make_frame(0x789, { 0x02 })).has_value(), "a frame goes out on one bus");

    helpers::CanFrame received[4];
    auto leaked = (*there)->receive(received, can::Duration { 20 });
    check(leaked.has_value() && *leaked == 0, "and does not appear on the other");

    can::virtual_bus_inject("here", make_frame(0x1, { 0x03 }));
    auto leakedInjection = (*there)->receive(received, can::Duration { 20 });
    check(leakedInjection.has_value() && *leakedInjection == 0, "nor does an injected one");
}

void test_virtual_bus_rejects_bad_frames()
{
    auto registry = make_virtual_only_registry();
    auto channel = registry.open("virtual:limits", can::OpenOptions {});
    check(channel.has_value(), "the channel opens");
    if (!channel.has_value())
    {
        return;
    }

    // An 11-bit frame cannot carry a 29-bit identifier, and sending it anyway
    // would put a different message on the bus from the one asked for.
    helpers::CanFrame tooBig {};
    tooBig.id = 0x1FFFF;
    tooBig.isExtended = false;
    auto result = (*channel)->send(tooBig);
    check(!result.has_value(), "an identifier too wide for its frame format is refused");
    check(!result.has_value() && result.error().kind == can::Error::Kind::InvalidArgument,
          "as an invalid argument");

    tooBig.isExtended = true;
    check((*channel)->send(tooBig).has_value(), "the same identifier is fine in an extended frame");

    // A stopped channel does not transmit.
    check((*channel)->stop().has_value(), "the channel stops");
    check(!(*channel)->send(make_frame(0x1, { 0 })).has_value(),
          "and a stopped channel refuses to send");
    check((*channel)->statistics().state == can::BusState::Stopped, "and reports itself stopped");
}

void test_virtual_bus_across_threads()
{
    // send() has to be safe while another thread is blocked in receive(); the
    // bridge node does exactly this, one reader thread per channel with
    // senders arriving on zenoh callback threads.
    auto registry = make_virtual_only_registry();
    auto reader = registry.open("virtual:threads", can::OpenOptions {});
    auto writer = registry.open("virtual:threads", can::OpenOptions {});
    if (!reader.has_value() || !writer.has_value())
    {
        check(false, "both channels open");
        return;
    }

    constexpr int kFrames = 200;
    std::thread sender(
        [&]
        {
            for (int i = 0; i < kFrames; ++i)
            {
                (void)(*writer)->send(make_frame(static_cast<uint32_t>(0x100 + (i % 8)), { 0x5A }));
            }
        });

    int received = 0;
    helpers::CanFrame batch[16];
    while (received < kFrames)
    {
        auto count = (*reader)->receive(batch, can::Duration { 500 });
        if (!count.has_value() || *count == 0)
        {
            break;
        }
        received += static_cast<int>(*count);
    }
    sender.join();

    check(received == kFrames,
          fmt::format("every one of {} frames sent from another thread arrived (got {})", kFrames,
                      received));
}

void test_registry_reports_unknown_backends()
{
    auto registry = make_virtual_only_registry();
    auto result = registry.open("pcan:0", can::OpenOptions {});
    check(!result.has_value(), "a backend this registry does not have cannot be opened");
    check(!result.has_value() && result.error().kind == can::Error::Kind::NotFound,
          "and says so as NotFound");
    check(!result.has_value() && result.error().message.find("virtual") != std::string::npos,
          "listing what it does have, so the message is actionable");

    auto malformed = registry.open("nonsense", can::OpenOptions {});
    check(!malformed.has_value()
              && malformed.error().kind == can::Error::Kind::InvalidArgument,
          "a malformed id fails as an invalid argument before any backend is consulted");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_standard_rates_are_exact();
    test_sample_point_lands_where_asked();
    test_fd_data_phase();
    test_impossible_rates_are_refused();
    test_timing_at_other_clocks();

    test_dlc();
    test_channel_id_parsing();

    test_virtual_bus();
    test_virtual_buses_are_separate();
    test_virtual_bus_rejects_bad_frames();
    test_virtual_bus_across_threads();
    test_registry_reports_unknown_backends();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all CAN core checks passed");
    return 0;
}
