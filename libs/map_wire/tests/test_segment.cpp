// SPDX-License-Identifier: GPL-3.0-or-later
//
// The graph-to-wire mapping, value by value.
//
// Both enumerations are parallel to their wire counterparts today, so a
// static_cast would produce the right answer -- right up until someone inserts
// a value into one of them, at which point every road reports the class of its
// neighbour and every speed limit reports the wrong provenance. Neither throws
// and neither shows up in a screenshot.
//
// So this checks every value rather than a couple. It is the counterpart to
// -Wswitch-enum: the switch makes adding a value a build failure, and this
// makes reordering one a test failure.

#include "map_wire/segment.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

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

void test_every_road_class_maps_to_its_own_wire_value()
{
    const std::vector<std::pair<map_rules::RouteClass, ::MapRoadClass>> expected {
        { map_rules::RouteClass::None, ::MapRoadClass::UNKNOWN },
        { map_rules::RouteClass::Motorway, ::MapRoadClass::MOTORWAY },
        { map_rules::RouteClass::Trunk, ::MapRoadClass::TRUNK },
        { map_rules::RouteClass::Primary, ::MapRoadClass::PRIMARY },
        { map_rules::RouteClass::Secondary, ::MapRoadClass::SECONDARY },
        { map_rules::RouteClass::Tertiary, ::MapRoadClass::TERTIARY },
        { map_rules::RouteClass::Minor, ::MapRoadClass::MINOR },
        { map_rules::RouteClass::Service, ::MapRoadClass::SERVICE },
        { map_rules::RouteClass::Track, ::MapRoadClass::TRACK },
        { map_rules::RouteClass::Path, ::MapRoadClass::PATH },
        { map_rules::RouteClass::Pedestrian, ::MapRoadClass::PEDESTRIAN },
        { map_rules::RouteClass::Ferry, ::MapRoadClass::FERRY },
    };

    for (const auto& [from, to] : expected)
    {
        check(map_wire::classOf(from) == to,
              "route class " + std::to_string(static_cast<int>(from)) + " maps to " +
                  std::to_string(static_cast<int>(to)));
        // The byte form a SegmentRecord stores must agree with the typed one.
        check(map_wire::classOf(static_cast<std::uint8_t>(from)) == to,
              "and so does the raw byte form");
    }

    // A value from outside the enum -- a graph written by a newer build --
    // must land on `unknown` rather than on whatever it numerically hits.
    check(map_wire::classOf(static_cast<std::uint8_t>(200)) == ::MapRoadClass::UNKNOWN,
          "an unrecognised class reports unknown rather than a plausible road");
}

void test_every_speed_source_maps_to_its_own_wire_value()
{
    // This one used to be a static_cast at both call sites. `sign` is the only
    // value a dash may show a driver as a posted limit, so mislabelling a
    // guess as a sign is the failure that matters.
    const std::vector<std::pair<map_rules::SpeedSource, ::MapSpeedSource>> expected {
        { map_rules::SpeedSource::Unknown, ::MapSpeedSource::UNKNOWN },
        { map_rules::SpeedSource::Sign, ::MapSpeedSource::SIGN },
        { map_rules::SpeedSource::ImplicitUrban, ::MapSpeedSource::IMPLICIT_URBAN },
        { map_rules::SpeedSource::ImplicitRural, ::MapSpeedSource::IMPLICIT_RURAL },
        { map_rules::SpeedSource::ImplicitClass, ::MapSpeedSource::IMPLICIT_CLASS },
        { map_rules::SpeedSource::ConditionalIgnored, ::MapSpeedSource::CONDITIONAL_IGNORED },
    };

    for (const auto& [from, to] : expected)
    {
        check(map_wire::speedSourceOf(from) == to,
              "speed source " + std::to_string(static_cast<int>(from)) + " maps to " +
                  std::to_string(static_cast<int>(to)));
        check(map_wire::speedSourceOf(static_cast<std::uint8_t>(from)) == to,
              "and so does the raw byte form");
    }

    check(map_wire::speedSourceOf(static_cast<std::uint8_t>(200)) == ::MapSpeedSource::UNKNOWN,
          "an unrecognised source reports unknown rather than claiming a sign");
}

void test_a_speed_without_a_posted_flag_says_so()
{
    ::capnp::MallocMessageBuilder message;
    auto speed = message.initRoot<::MapSpeed>();

    road_graph::SegmentRecord segment {};
    segment.postedSpeedKph = 0;
    segment.freeFlowSpeedKph = 48;
    segment.postedSource = static_cast<std::uint8_t>(map_rules::SpeedSource::ImplicitClass);
    segment.flags = 0;

    map_wire::fillSpeed(speed, segment);

    // ZERO is a legal posted limit -- a barrier, a gate -- so hasPosted must
    // come from the flag and not from the number being non-zero. A consumer
    // that inferred it from postedKph would show nothing at exactly the places
    // where the limit matters most.
    check(!speed.asReader().getHasPosted(), "a segment with no posted flag reports none");
    check(speed.asReader().getFreeFlowKph() == 48, "and still carries the free-flow speed");
    check(speed.asReader().getPostedSource() == ::MapSpeedSource::IMPLICIT_CLASS,
          "and says where the number came from");

    segment.flags = road_graph::kFlagHasPosted;
    segment.postedSpeedKph = 0;
    segment.postedSource = static_cast<std::uint8_t>(map_rules::SpeedSource::Sign);
    map_wire::fillSpeed(speed, segment);
    check(speed.asReader().getHasPosted(), "a posted limit of ZERO is still a posted limit");
    check(speed.asReader().getPostedKph() == 0, "with the zero carried through");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_every_road_class_maps_to_its_own_wire_value();
    test_every_speed_source_maps_to_its_own_wire_value();
    test_a_speed_without_a_posted_flag_says_so();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all map_wire checks passed");
    return 0;
}
