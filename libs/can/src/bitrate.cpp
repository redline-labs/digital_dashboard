// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/bitrate.h"

#include <spdlog/fmt/fmt.h>

#include <cstdlib>
#include <limits>

namespace can
{
namespace
{

// CiA 301 allows a node to be this far off the nominal rate and still stay on
// the bus. Anything worse is not a bit rate the adapter can generate, and
// saying so is better than handing back a number that nearly works.
constexpr uint32_t kMaxErrorPermille = 5;

uint32_t abs_diff(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

} // namespace

uint16_t default_sample_point_permille(uint32_t bitrateBps)
{
    // The same thresholds the Linux CAN core uses, so an adapter driven by
    // this library ends up sampling where a SocketCAN node on the same bus
    // does. Faster bits leave less room for propagation delay, so the sample
    // point moves earlier.
    if (bitrateBps > 800000)
    {
        return 750;
    }
    if (bitrateBps > 500000)
    {
        return 800;
    }
    return 875;
}

std::string Bitrate::toString() const
{
    auto rate = [](uint32_t bps)
    {
        if (bps >= 1000000 && bps % 1000000 == 0)
        {
            return fmt::format("{} Mbit/s", bps / 1000000);
        }
        if (bps >= 1000 && bps % 1000 == 0)
        {
            return fmt::format("{} kbit/s", bps / 1000);
        }
        return fmt::format("{} bit/s", bps);
    };

    if (!fd())
    {
        return rate(nominalBps);
    }
    return fmt::format("{} + {} data", rate(nominalBps), rate(dataBps));
}

std::string BitTiming::toString() const
{
    return fmt::format("brp={} tseg1={} tseg2={} sjw={} ({} tq -> {} bit/s, sample {}.{}%)", brp,
                       tseg1, tseg2, sjw, tq(), bitrateBps, samplePointPermille / 10,
                       samplePointPermille % 10);
}

Result<BitTiming> solve_bit_timing(uint32_t bitrateBps, uint16_t samplePointPermille,
                                   const BitTimingLimits& limits)
{
    if (bitrateBps == 0)
    {
        return invalid_argument("a bit rate of 0 is not a bit rate");
    }
    if (limits.clockHz == 0)
    {
        return invalid_argument("the controller clock frequency is not known");
    }
    if (samplePointPermille == 0)
    {
        samplePointPermille = default_sample_point_permille(bitrateBps);
    }
    if (samplePointPermille >= 1000)
    {
        return invalid_argument(
            fmt::format("a sample point of {}.{}% is not inside the bit",
                        samplePointPermille / 10, samplePointPermille % 10));
    }

    BitTiming best {};
    uint32_t bestRateError = std::numeric_limits<uint32_t>::max();
    uint32_t bestSampleError = std::numeric_limits<uint32_t>::max();
    bool found = false;

    // Longest bit first. More time quanta per bit means finer control over
    // where the sample point lands, so among timings that hit the rate exactly
    // the longest one is the most accurate about everything else.
    const uint32_t tqMax = 1u + limits.tseg1Max + limits.tseg2Max;
    const uint32_t tqMin = 1u + limits.tseg1Min + limits.tseg2Min;

    for (uint32_t tq = tqMax; tq >= tqMin; --tq)
    {
        // brp = clock / (bitrate * tq), rounded to nearest.
        const uint64_t denominator = static_cast<uint64_t>(bitrateBps) * tq;
        if (denominator == 0)
        {
            continue;
        }
        const uint64_t brp = (static_cast<uint64_t>(limits.clockHz) + denominator / 2) / denominator;
        if (brp < limits.brpMin || brp > limits.brpMax)
        {
            continue;
        }

        const uint32_t actual
            = static_cast<uint32_t>(static_cast<uint64_t>(limits.clockHz) / (brp * tq));
        const uint32_t rateError = abs_diff(actual, bitrateBps);

        // Split the programmable quanta so the sample point lands as close to
        // the request as the segment limits allow.
        //
        // tseg1 covers everything before the sample point except the one-quantum
        // sync segment, so the ideal is (samplePoint * tq / 1000) - 1.
        int64_t tseg1 = (static_cast<int64_t>(samplePointPermille) * tq) / 1000 - 1;
        if (tseg1 < limits.tseg1Min)
        {
            tseg1 = limits.tseg1Min;
        }
        if (tseg1 > limits.tseg1Max)
        {
            tseg1 = limits.tseg1Max;
        }

        int64_t tseg2 = static_cast<int64_t>(tq) - 1 - tseg1;
        if (tseg2 < limits.tseg2Min)
        {
            // Give the quanta back to tseg1 rather than abandoning this tq.
            tseg2 = limits.tseg2Min;
            tseg1 = static_cast<int64_t>(tq) - 1 - tseg2;
        }
        if (tseg2 > limits.tseg2Max)
        {
            tseg2 = limits.tseg2Max;
            tseg1 = static_cast<int64_t>(tq) - 1 - tseg2;
        }
        if (tseg1 < limits.tseg1Min || tseg1 > limits.tseg1Max)
        {
            continue;
        }

        const uint16_t actualSamplePoint
            = static_cast<uint16_t>(((1 + tseg1) * 1000) / static_cast<int64_t>(tq));
        const uint32_t sampleError = abs_diff(actualSamplePoint, samplePointPermille);

        // Rate first, sample point second. A sample point a few per cent out
        // still communicates; a bit rate a few per cent out does not.
        const bool better = !found || rateError < bestRateError
            || (rateError == bestRateError && sampleError < bestSampleError);
        if (!better)
        {
            continue;
        }

        found = true;
        bestRateError = rateError;
        bestSampleError = sampleError;

        best.brp = static_cast<uint32_t>(brp);
        best.tseg1 = static_cast<uint16_t>(tseg1);
        best.tseg2 = static_cast<uint16_t>(tseg2);
        // The resynchronisation jump width cannot exceed tseg2, and a wider
        // one tolerates more clock drift between nodes, so take the widest
        // allowed.
        best.sjw = static_cast<uint16_t>(std::min<uint32_t>(limits.sjwMax, best.tseg2));
        if (best.sjw == 0)
        {
            best.sjw = 1;
        }
        best.bitrateBps = actual;
        best.samplePointPermille = actualSamplePoint;

        if (rateError == 0 && sampleError == 0)
        {
            break;
        }
    }

    if (!found)
    {
        return unsupported(fmt::format(
            "no bit timing for {} bit/s from a {} Hz clock within the controller's limits",
            bitrateBps, limits.clockHz));
    }

    const uint64_t errorPermille
        = (static_cast<uint64_t>(bestRateError) * 1000) / bitrateBps;
    if (errorPermille > kMaxErrorPermille)
    {
        return unsupported(fmt::format(
            "the closest bit timing to {} bit/s from a {} Hz clock is {} bit/s, which is {}.{}% "
            "off -- more than the 0.5% a CAN bus tolerates",
            bitrateBps, limits.clockHz, best.bitrateBps, errorPermille / 10, errorPermille % 10));
    }

    return best;
}

} // namespace can
