// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning "500 kbit/s" into the register values a CAN controller actually
// takes.
//
// A CAN bit is divided into time quanta: one fixed sync segment, then two
// programmable segments whose boundary is where the bus is sampled.
//
//        |<---------------- one bit ---------------->|
//        | sync |     tseg1      |      tseg2        |
//        |  1tq |                ^                   |
//                          sample point
//
// The quantum itself is the controller clock divided by a prescaler, so
//
//     bitrate = clock / (brp * (1 + tseg1 + tseg2))
//
// Two things make this worth solving properly rather than looking up in a
// table. The sample point has to land where the rest of the bus puts it -- get
// it wrong and the link works on the bench and fails on a long harness, which
// is the worst failure mode available. And CAN FD needs the whole calculation
// twice against different constraints, because the data phase has a much
// smaller tseg range.
//
// Everything here is integer arithmetic on a target's declared constraints, so
// it is testable without an adapter, and it is: the tests check that the
// standard rates come out exact at 80 MHz.
#ifndef CAN_BITRATE_H
#define CAN_BITRATE_H

#include "can/error.h"

#include <cstdint>
#include <string>

namespace can
{

// What the caller wants.
struct Bitrate
{
    uint32_t nominalBps { 500000 };
    // The CAN FD data-phase rate. Zero means classic CAN: no data phase, no
    // bit-rate switching, eight bytes maximum.
    uint32_t dataBps { 0 };

    // Where in the bit to sample, in per mille (875 = 87.5%). Zero asks for
    // the CiA default for the rate, which is what everything else on a vehicle
    // bus will be using.
    uint16_t nominalSamplePointPermille { 0 };
    uint16_t dataSamplePointPermille { 0 };

    bool fd() const { return dataBps != 0; }

    bool operator==(const Bitrate& other) const = default;

    // "500 kbit/s" or "500 kbit/s + 2 Mbit/s data".
    std::string toString() const;
};

// The CiA default sample point for a rate, in per mille. Slower buses sample
// later because propagation delay is a smaller fraction of the bit.
uint16_t default_sample_point_permille(uint32_t bitrateBps);

// What a particular controller can generate.
struct BitTimingLimits
{
    uint32_t clockHz { 80000000 };
    uint16_t tseg1Min { 1 };
    uint16_t tseg1Max { 64 };
    uint16_t tseg2Min { 1 };
    uint16_t tseg2Max { 16 };
    uint16_t sjwMax { 16 };
    uint32_t brpMin { 1 };
    uint32_t brpMax { 1024 };
};

// The answer.
struct BitTiming
{
    uint32_t brp { 1 };
    uint16_t tseg1 { 0 };
    uint16_t tseg2 { 0 };
    uint16_t sjw { 1 };

    // What the numbers above actually produce, as opposed to what was asked
    // for. A caller that cares should compare them: a bus where one node is
    // 0.5% off is a bus that works until it is warm.
    uint32_t bitrateBps { 0 };
    uint16_t samplePointPermille { 0 };

    // Time quanta in one bit, including the sync segment.
    uint16_t tq() const { return static_cast<uint16_t>(1 + tseg1 + tseg2); }

    std::string toString() const;
};

// Solves for the timing closest to `bitrateBps` at `samplePointPermille`,
// preferring an exact bit rate over an exact sample point -- a sample point a
// few per cent off still communicates, a bit rate a few per cent off does not.
//
// Fails when no combination within the limits comes within 0.5% of the
// requested rate, which is the tolerance CiA 301 allows across a bus.
Result<BitTiming> solve_bit_timing(uint32_t bitrateBps, uint16_t samplePointPermille,
                                   const BitTimingLimits& limits);

} // namespace can

#endif // CAN_BITRATE_H
