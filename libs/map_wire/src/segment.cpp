// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_wire/segment.h"

namespace map_wire
{

::MapRoadClass classOf(map_rules::RouteClass value)
{
    switch (value)
    {
        case map_rules::RouteClass::None:
            return ::MapRoadClass::UNKNOWN;
        case map_rules::RouteClass::Motorway:
            return ::MapRoadClass::MOTORWAY;
        case map_rules::RouteClass::Trunk:
            return ::MapRoadClass::TRUNK;
        case map_rules::RouteClass::Primary:
            return ::MapRoadClass::PRIMARY;
        case map_rules::RouteClass::Secondary:
            return ::MapRoadClass::SECONDARY;
        case map_rules::RouteClass::Tertiary:
            return ::MapRoadClass::TERTIARY;
        case map_rules::RouteClass::Minor:
            return ::MapRoadClass::MINOR;
        case map_rules::RouteClass::Service:
            return ::MapRoadClass::SERVICE;
        case map_rules::RouteClass::Track:
            return ::MapRoadClass::TRACK;
        case map_rules::RouteClass::Path:
            return ::MapRoadClass::PATH;
        case map_rules::RouteClass::Pedestrian:
            return ::MapRoadClass::PEDESTRIAN;
        case map_rules::RouteClass::Ferry:
            return ::MapRoadClass::FERRY;
    }

    // After the switch rather than in a default:, so adding a class stays a
    // compile error.
    return ::MapRoadClass::UNKNOWN;
}

::MapRoadClass classOf(std::uint8_t routeClass)
{
    return classOf(static_cast<map_rules::RouteClass>(routeClass));
}

::MapSpeedSource speedSourceOf(map_rules::SpeedSource value)
{
    switch (value)
    {
        case map_rules::SpeedSource::Unknown:
            return ::MapSpeedSource::UNKNOWN;
        case map_rules::SpeedSource::Sign:
            return ::MapSpeedSource::SIGN;
        case map_rules::SpeedSource::ImplicitUrban:
            return ::MapSpeedSource::IMPLICIT_URBAN;
        case map_rules::SpeedSource::ImplicitRural:
            return ::MapSpeedSource::IMPLICIT_RURAL;
        case map_rules::SpeedSource::ImplicitClass:
            return ::MapSpeedSource::IMPLICIT_CLASS;
        case map_rules::SpeedSource::ConditionalIgnored:
            return ::MapSpeedSource::CONDITIONAL_IGNORED;
    }

    return ::MapSpeedSource::UNKNOWN;
}

::MapSpeedSource speedSourceOf(std::uint8_t postedSource)
{
    return speedSourceOf(static_cast<map_rules::SpeedSource>(postedSource));
}

void fillSpeed(::MapSpeed::Builder speed, const road_graph::SegmentRecord& segment)
{
    speed.setHasPosted((segment.flags & road_graph::kFlagHasPosted) != 0);
    speed.setPostedKph(segment.postedSpeedKph);
    speed.setPostedSource(speedSourceOf(segment.postedSource));
    speed.setFreeFlowKph(segment.freeFlowSpeedKph);
}

} // namespace map_wire
