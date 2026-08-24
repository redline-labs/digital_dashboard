// SPDX-License-Identifier: GPL-3.0-or-later
//
// Assembling one fix from the per-record topics, at whatever rates they arrive.
//
// The bridge publishes one topic per GSOF record type and fuses nothing. This
// matcher needs three of them -- position (2), velocity (8) and accuracy (12)
// -- and none of those records carries a time, so something has to decide which
// values belong with which position.
//
// PAIRING IS BY ARRIVAL AGE, NOT BY BATCH MEMBERSHIP, and the difference
// matters as soon as the receiver stops running everything at one rate. A
// receiver is normally configured with position fast and status slow -- 50 Hz
// position against 1 Hz accuracy is a reasonable setup -- and then:
//
//   * Batch membership (which records shared a GSOF transmission) throws away
//     an accuracy record that arrived 20 ms ago simply because a newer position
//     started a new transmission. At 50 Hz that value is perfectly good, and
//     discarding it means running on the default sigma 49 times in 50.
//   * Batch membership also breaks silently when someone changes a rate. The
//     consumer keeps working and quietly pairs less often, and nothing says so.
//
// Age answers the question actually being asked -- "is this value still
// describing the same moment as this position?" -- and it answers it the same
// way whether the receiver runs at 1 Hz or 50 Hz, whether the two records share
// a rate, and whether the transmission structure changes. Records that ARE
// co-scheduled arrive microseconds apart, so they pair with room to spare;
// records at genuinely different rates pair when they are fresh enough and are
// reported absent when they are not.
//
// WHAT IS DELIBERATELY NOT DONE: a stale value is never used. Absence is a
// state the matcher handles -- it falls back to distance alone without a
// heading, and to a configured sigma without an accuracy -- and that is far
// better than steering by a heading from half a second ago, which through a
// turn is a confidently wrong match rather than a visible fault.
//
// Deliberately free of zenoh and capnp so the rule can be tested on its own.
// Every failure here produces a plausible wrong answer rather than an error.

#ifndef MAP_MATCH_FIX_ASSEMBLER_H
#define MAP_MATCH_FIX_ASSEMBLER_H

#include <chrono>
#include <cstdint>
#include <optional>

namespace map_match
{

// A value and when it arrived. Absent until the first one does.
template <typename T>
struct Timed
{
    std::optional<T> value;
    std::chrono::steady_clock::time_point at {};

    void set(const T& newValue, std::chrono::steady_clock::time_point when)
    {
        value = newValue;
        at = when;
    }

    // Fresh enough to describe the same moment as something at `now`.
    bool freshAt(std::chrono::steady_clock::time_point now, std::chrono::milliseconds within) const
    {
        return value.has_value() && (now - at) <= within;
    }
};

struct VelocitySample
{
    bool valid { false };
    float headingDeg { 0.0F };
    float speedMps { 0.0F };
};

// What the matcher gets for one position.
struct AssembledFix
{
    double latitudeDeg { 0.0 };
    double longitudeDeg { 0.0 };

    bool hasVelocity { false };
    bool velocityValid { false };
    float headingDeg { 0.0F };
    float speedMps { 0.0F };

    bool hasSigma { false };
    float positionRmsM { 0.0F };
};

class FixAssembler
{
  public:
    struct Counts
    {
        std::uint64_t fixes { 0 };
        // Positions that found no velocity fresh enough to use. Persistently
        // high means velocity is disabled, or is running far slower than
        // position -- a configuration fact, and one worth publishing rather
        // than absorbing into a default.
        std::uint64_t withoutVelocity { 0 };
        std::uint64_t withoutSigma { 0 };
    };

    explicit FixAssembler(std::chrono::milliseconds pairWithin) : mPairWithin(pairWithin) {}

    void setVelocity(const VelocitySample& sample, std::chrono::steady_clock::time_point at)
    {
        mVelocity.set(sample, at);
    }

    void setSigma(float positionRmsM, std::chrono::steady_clock::time_point at)
    {
        mSigma.set(positionRmsM, at);
    }

    // A position completes a fix: it is the record the matcher runs on, and the
    // others are context for it. Anything not fresh enough is left absent.
    AssembledFix onPosition(double latitudeDeg, double longitudeDeg,
                            std::chrono::steady_clock::time_point at)
    {
        AssembledFix fix;
        fix.latitudeDeg = latitudeDeg;
        fix.longitudeDeg = longitudeDeg;

        ++mCounts.fixes;

        if (mVelocity.freshAt(at, mPairWithin))
        {
            fix.hasVelocity = true;
            fix.velocityValid = mVelocity.value->valid;
            fix.headingDeg = mVelocity.value->headingDeg;
            fix.speedMps = mVelocity.value->speedMps;
        }
        else
        {
            ++mCounts.withoutVelocity;
        }

        if (mSigma.freshAt(at, mPairWithin))
        {
            fix.hasSigma = true;
            fix.positionRmsM = *mSigma.value;
        }
        else
        {
            ++mCounts.withoutSigma;
        }

        return fix;
    }

    const Counts& counts() const { return mCounts; }

  private:
    std::chrono::milliseconds mPairWithin;
    Timed<VelocitySample> mVelocity;
    Timed<float> mSigma;
    Counts mCounts {};
};

} // namespace map_match

#endif // MAP_MATCH_FIX_ASSEMBLER_H
