// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pairing the per-record GSOF topics into one fix, at whatever rates they come.
//
// Every failure here produces a plausible wrong answer rather than an error: a
// heading paired with a position it does not describe still matches a road, and
// still renders, and is simply the wrong road. So the rule is a separate,
// zenoh-free thing and it gets its own test.
//
// The cases that matter are the ones a receiver reconfiguration creates. A
// receiver is normally set up with position fast and status slow, and the rates
// change whenever someone changes their mind about what they need.

#include "fix_assembler.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <string>

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

using map_match::AssembledFix;
using map_match::FixAssembler;
using map_match::VelocitySample;
using Clock = std::chrono::steady_clock;

constexpr auto kPairWithin = std::chrono::milliseconds(200);

// A fixed origin, so a test says "40 ms later" rather than depending on how
// long it took to run.
Clock::time_point at(int ms)
{
    return Clock::time_point {} + std::chrono::milliseconds(1'000'000 + ms);
}

VelocitySample moving(float headingDeg)
{
    return VelocitySample { true, headingDeg, 20.0F };
}

void test_records_sent_together_pair_with_room_to_spare()
{
    // The bridge publishes a transmission's records back to back, so records
    // the receiver co-schedules arrive microseconds apart. That has to pair
    // regardless of what the shared rate is -- 1 Hz and 50 Hz look identical
    // here, which is the whole point of pairing on age rather than on rate.
    FixAssembler assembler(kPairWithin);

    assembler.setVelocity(moving(90.0F), at(0));
    assembler.setSigma(0.25F, at(0));
    const AssembledFix fix = assembler.onPosition(33.0, -117.0, at(0));

    check(fix.hasVelocity && fix.velocityValid, "a co-published velocity pairs");
    check(fix.headingDeg == 90.0F, "with its own heading");
    check(fix.hasSigma && fix.positionRmsM == 0.25F, "and so does the accuracy");
    check(assembler.counts().fixes == 1, "one fix");
    check(assembler.counts().withoutVelocity == 0 && assembler.counts().withoutSigma == 0,
          "and nothing counted as missing");
}

void test_a_slow_record_pairs_while_fresh_and_is_absent_after()
{
    // 50 Hz position against a slower accuracy record. The accuracy is good for
    // as long as it describes the same moment, and then it is not -- and the
    // matcher gets absence rather than a stale number, because it handles
    // absence by falling back to a configured sigma.
    FixAssembler assembler(kPairWithin);

    assembler.setSigma(0.25F, at(0));

    check(assembler.onPosition(33.0, -117.0, at(20)).hasSigma, "20 ms later it still describes it");
    check(assembler.onPosition(33.0, -117.0, at(200)).hasSigma, "and at the boundary it still does");
    check(!assembler.onPosition(33.0, -117.0, at(201)).hasSigma, "one millisecond past, it does not");
    check(assembler.counts().withoutSigma == 1, "and that is counted, not absorbed");
}

void test_a_stale_heading_is_dropped_rather_than_used()
{
    // THE FAILURE THIS RULE EXISTS TO PREVENT. Half a second is long enough for
    // a vehicle to have turned, and a heading from before the turn matches
    // confidently onto the road the vehicle used to be on.
    FixAssembler assembler(kPairWithin);

    assembler.setVelocity(moving(90.0F), at(0));
    const AssembledFix fix = assembler.onPosition(33.0, -117.0, at(500));

    check(!fix.hasVelocity, "a half-second-old heading is not used");
    check(fix.headingDeg == 0.0F, "and the field is not left holding it either");
    check(fix.latitudeDeg == 33.0, "while the position itself is untouched");
    check(assembler.counts().withoutVelocity == 1, "the drop is counted");
}

void test_a_fresh_record_replaces_a_stale_one()
{
    // The held value is latest-known, so a record arriving after a gap makes
    // the pairing work again with no reset and no special case.
    FixAssembler assembler(kPairWithin);

    assembler.setVelocity(moving(90.0F), at(0));
    check(!assembler.onPosition(33.0, -117.0, at(500)).hasVelocity, "stale, as before");

    assembler.setVelocity(moving(270.0F), at(510));
    const AssembledFix fix = assembler.onPosition(33.0, -117.0, at(520));

    check(fix.hasVelocity, "the new one pairs");
    check(fix.headingDeg == 270.0F, "and it is the NEW heading, not the one it replaced");
}

void test_a_position_before_any_velocity_is_still_a_fix()
{
    // Startup, and the case where velocity is simply not enabled on the
    // receiver. The matcher must still run -- it falls back on distance alone,
    // which is a worse match but a match.
    FixAssembler assembler(kPairWithin);

    const AssembledFix fix = assembler.onPosition(33.0, -117.0, at(0));

    check(!fix.hasVelocity && !fix.hasSigma, "nothing to pair with, and nothing invented");
    check(fix.latitudeDeg == 33.0 && fix.longitudeDeg == -117.0, "the position stands on its own");
    check(assembler.counts().fixes == 1, "and it counts as a fix");
    check(assembler.counts().withoutVelocity == 1 && assembler.counts().withoutSigma == 1,
          "with both absences counted -- which is what makes a disabled record visible");
}

void test_an_invalid_velocity_is_not_the_same_as_an_absent_one()
{
    // A stationary vehicle publishes a velocity record that says it has no
    // usable heading. That is a real state, and it is not the same as the
    // receiver having stopped sending record 8 -- the matcher distinguishes
    // them, so the pairing must not flatten one into the other.
    FixAssembler assembler(kPairWithin);

    assembler.setVelocity(VelocitySample { false, 0.0F, 0.0F }, at(0));
    const AssembledFix fix = assembler.onPosition(33.0, -117.0, at(0));

    check(fix.hasVelocity, "the record arrived");
    check(!fix.velocityValid, "and says its heading is not usable");
    check(assembler.counts().withoutVelocity == 0, "which is not a pairing failure");
}

void test_velocity_is_reused_across_several_faster_positions()
{
    // 50 Hz position against 10 Hz velocity: several positions share one
    // velocity, and each of them should get it. A rule that consumed the held
    // value would leave four positions in five with no heading, which is the
    // behaviour grouping by GSOF transmission would have given.
    FixAssembler assembler(kPairWithin);

    assembler.setVelocity(moving(45.0F), at(0));
    for (int i = 0; i < 5; ++i)
    {
        const AssembledFix fix = assembler.onPosition(33.0, -117.0, at(20 * i));
        check(fix.hasVelocity && fix.headingDeg == 45.0F,
              "every position within the window gets the heading");
    }
    check(assembler.counts().fixes == 5, "five fixes");
    check(assembler.counts().withoutVelocity == 0, "none of them missing a heading");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    test_records_sent_together_pair_with_room_to_spare();
    test_a_slow_record_pairs_while_fresh_and_is_absent_after();
    test_a_stale_heading_is_dropped_rather_than_used();
    test_a_fresh_record_replaces_a_stale_one();
    test_a_position_before_any_velocity_is_still_a_fix();
    test_an_invalid_velocity_is_not_the_same_as_an_absent_one();
    test_velocity_is_reused_across_several_faster_positions();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all fix-assembler checks passed");
    return 0;
}
