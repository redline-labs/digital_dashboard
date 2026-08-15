// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rule that decides which GSOF records describe the same instant.
//
// Every case here is one that produces a PLAUSIBLE WRONG ANSWER rather than a
// failure, which is what makes it worth a test. A heading paired with the
// wrong position is not a dropout: it is a heading off by however far the
// vehicle turned in between, and it puts the vehicle on the frontage road
// beside the freeway with the map still rendering perfectly.

#include <optional>

#include <spdlog/spdlog.h>

#include "epoch.h"

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

gsof::LatLongHeight positionAt(double latRad, double lonRad)
{
    gsof::LatLongHeight out;
    out.latitudeRad = latRad;
    out.longitudeRad = lonRad;
    out.heightM = 42.0;
    return out;
}

gsof::Velocity velocityAt(float headingRad)
{
    gsof::Velocity out;
    // Bit 0 is the validity bit; without it the record says it has nothing.
    out.velocityFlags = 0x01;
    out.horizontalSpeedMps = 20.0F;
    out.headingRad = headingRad;
    return out;
}

gsof::PositionTime timeAt(std::uint32_t timeOfWeekMs)
{
    gsof::PositionTime out;
    out.gpsWeek = 2400;
    out.gpsTimeMs = timeOfWeekMs;
    out.svsUsed = 14;
    return out;
}

void test_a_transmission_carrying_a_position_yields_an_epoch()
{
    bd992_node::EpochAccumulator acc;

    acc.add(positionAt(0.5, -2.0));
    acc.add(velocityAt(1.0F));
    acc.add(timeAt(1000));

    const std::optional<bd992_node::FusedEpoch> epoch = acc.take();

    check(epoch.has_value(), "a transmission with a position yields an epoch");
    if (!epoch.has_value())
    {
        return;
    }

    check(epoch->position.latitudeRad == 0.5, "and it carries the position it was given");
    check(epoch->velocity.has_value(), "and the velocity that came with it");
    check(epoch->time.has_value(), "and the time that came with it");
    check(epoch->fixType == std::nullopt, "and nothing it was not given");
}

void test_a_transmission_without_a_position_yields_nothing()
{
    bd992_node::EpochAccumulator acc;

    // Legitimate: status records keep their own schedule. The velocity here is
    // deliberately valid -- there is simply nothing for it to be about.
    acc.add(velocityAt(1.0F));
    acc.add(timeAt(1000));

    check(!acc.take().has_value(), "a transmission with no position is not an epoch");
    check(acc.counts().withoutPosition == 1, "and it is counted as such");
    check(acc.counts().epochs == 0, "and no epoch is counted");
}

void test_nothing_survives_into_the_next_transmission()
{
    // THE case this class exists for. A velocity that arrives in one
    // transmission must not be attached to a position in the next: the
    // resulting heading is stale by one output interval and is indistinguishable
    // from a real one.
    bd992_node::EpochAccumulator acc;

    acc.add(positionAt(0.5, -2.0));
    acc.add(velocityAt(1.0F));
    const std::optional<bd992_node::FusedEpoch> first = acc.take();
    check(first.has_value() && first->velocity.has_value(), "the first epoch has its velocity");

    // Second transmission: a position only.
    acc.add(positionAt(0.6, -2.1));
    const std::optional<bd992_node::FusedEpoch> second = acc.take();

    check(second.has_value(), "the second transmission still yields an epoch");
    if (!second.has_value())
    {
        return;
    }

    check(!second->velocity.has_value(),
          "and it reports NO velocity rather than the previous transmission's");
    check(second->position.latitudeRad == 0.6, "and its own position");
}

void test_a_transmission_that_yielded_nothing_still_clears()
{
    // The other half of the same rule: a transmission with no position must
    // not leave its velocity lying about for the next one to pick up.
    bd992_node::EpochAccumulator acc;

    acc.add(velocityAt(1.0F));
    check(!acc.take().has_value(), "a position-less transmission yields nothing");

    acc.add(positionAt(0.5, -2.0));
    const std::optional<bd992_node::FusedEpoch> epoch = acc.take();

    check(epoch.has_value(), "the next transmission yields an epoch");
    check(epoch.has_value() && !epoch->velocity.has_value(),
          "and it did not inherit the orphaned velocity");
}

void test_absent_components_are_absent_rather_than_defaulted()
{
    bd992_node::EpochAccumulator acc;
    acc.add(positionAt(0.5, -2.0));

    const std::optional<bd992_node::FusedEpoch> epoch = acc.take();
    check(epoch.has_value(), "a position alone is an epoch");
    if (!epoch.has_value())
    {
        return;
    }

    // A defaulted GsofGpsTime is week 0, which is January 1980 and looks like a
    // real timestamp. Absence has to be representable.
    check(!epoch->time.has_value(), "an absent time is absent");
    check(!epoch->velocity.has_value(), "an absent velocity is absent");
    check(!epoch->fixType.has_value(), "an absent fix type is absent");
    check(!epoch->sigma.has_value(), "an absent sigma is absent");
}

void test_the_first_of_each_kind_wins_and_the_duplicate_is_counted()
{
    // A transmission carrying two epochs cannot be fused correctly by anyone.
    // What matters is that the behaviour is DEFINED and that the anomaly is
    // visible, rather than a silent mispairing.
    bd992_node::EpochAccumulator acc;

    acc.add(positionAt(0.5, -2.0));
    acc.add(velocityAt(1.0F));
    acc.add(positionAt(0.9, -2.9));
    acc.add(velocityAt(2.0F));

    const std::optional<bd992_node::FusedEpoch> epoch = acc.take();
    check(epoch.has_value(), "it still yields one epoch");
    if (!epoch.has_value())
    {
        return;
    }

    check(epoch->position.latitudeRad == 0.5, "built from the first position");
    check(epoch->velocity.has_value() && epoch->velocity->headingRad == 1.0F,
          "and the first velocity, so the two are from the same instant");
    check(acc.counts().duplicates == 2, "and both duplicates are counted");
}

void test_records_that_are_not_part_of_an_epoch_are_ignored()
{
    // add() is called for every record so the caller needs no filter. Records
    // outside the epoch must neither appear nor count as duplicates.
    bd992_node::EpochAccumulator acc;

    gsof::DopInfo dop;
    dop.pdop = 1.5F;

    acc.add(dop);
    check(!acc.take().has_value(), "a transmission of only unrelated records is not an epoch");
    check(acc.counts().duplicates == 0, "and nothing is counted as a duplicate");

    acc.add(dop);
    acc.add(positionAt(0.5, -2.0));
    acc.add(dop);

    check(acc.take().has_value(), "unrelated records do not prevent an epoch");
    check(acc.counts().duplicates == 0, "and repeating one is still not a duplicate");
}

void test_counts_account_for_every_transmission()
{
    bd992_node::EpochAccumulator acc;

    acc.add(positionAt(0.5, -2.0));
    (void)acc.take();
    (void)acc.take();
    acc.add(positionAt(0.6, -2.1));
    (void)acc.take();

    const bd992_node::EpochAccumulator::Counts counts = acc.counts();
    check(counts.transmissions == 3, "every take() is a transmission");
    check(counts.epochs == 2, "two of them carried a position");
    check(counts.withoutPosition == 1, "and one did not");
    check(counts.epochs + counts.withoutPosition == counts.transmissions,
          "and the two account for all of them");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_transmission_carrying_a_position_yields_an_epoch();
    test_a_transmission_without_a_position_yields_nothing();
    test_nothing_survives_into_the_next_transmission();
    test_a_transmission_that_yielded_nothing_still_clears();
    test_absent_components_are_absent_rather_than_defaulted();
    test_the_first_of_each_kind_wins_and_the_duplicate_is_counted();
    test_records_that_are_not_part_of_an_epoch_are_ignored();
    test_counts_account_for_every_transmission();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all epoch fusion checks passed");
    return 0;
}
