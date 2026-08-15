// SPDX-License-Identifier: GPL-3.0-or-later
//
// One tag set, two answers, and the places where they deliberately disagree.
//
// The cases that matter most are the asymmetric ones. A rule set that made
// "drawn" and "routable" the same thing would pass a naive test and then send a
// car down a footpath, or route along a road the map does not show. Each of
// those is asserted here explicitly, so that changing one side without the
// other is a test failure rather than a field report.

#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "map_rules/classification.h"

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

using Pairs = std::vector<std::pair<std::string_view, std::string_view>>;

map_rules::RoadClassification classify(const Pairs& pairs, bool closed = false)
{
    map_rules::TagView view { pairs };
    return map_rules::classify(view, map_rules::Shape { closed });
}

void test_a_motorway_is_drawn_and_routable()
{
    const auto result = classify({ { "highway", "motorway" }, { "name", "Costa Mesa Freeway" } });

    check(result.renderClass == map_rules::RenderClass::Motorway, "a motorway draws as a motorway");
    check(result.routeClass == map_rules::RouteClass::Motorway, "and routes as one");
    check(result.minZoom <= 6, "and appears at low zoom");
    check((result.access & map_rules::kAccessMotorcar) != 0, "cars may use it");
    check((result.access & map_rules::kAccessFoot) == 0, "pedestrians may not");
    check(result.onewayForward, "and a motorway carriageway is one-way even untagged");
    check(result.freeFlowSpeedKph > 80, "with a freeway free-flow speed");
    check(!result.hasPosted, "but no posted limit, because no sign was tagged");
}

void test_a_footway_is_drawn_and_not_driveable()
{
    // The asymmetry. A rule set with one enum cannot say this.
    const auto result = classify({ { "highway", "footway" } });

    check(result.drawn(), "a footway is drawn");
    check(result.renderClass == map_rules::RenderClass::Path, "as a path");
    check(result.routeClass == map_rules::RouteClass::Path, "and is routable");
    check((result.access & map_rules::kAccessFoot) != 0, "on foot");
    check((result.access & map_rules::kAccessMotorcar) == 0, "and NOT by car");
}

void test_a_service_road_is_driveable_before_it_is_drawn()
{
    // The other asymmetry, and the one that produces "the route goes down a
    // road that isn't on the map" if the two sides drift apart. A driveway is
    // routable everywhere and drawn only at z14.
    const auto result = classify({ { "highway", "service" } });

    check(result.routable(), "a service road is routable");
    check((result.access & map_rules::kAccessMotorcar) != 0, "by car");
    check(result.minZoom >= 14, "and is not drawn until z14");
}

void test_steps_are_walkable_but_not_cyclable()
{
    const auto result = classify({ { "highway", "steps" } });
    check((result.access & map_rules::kAccessFoot) != 0, "steps are walkable");
    check((result.access & map_rules::kAccessBicycle) == 0, "and not cyclable");
}

void test_access_tags_close_a_road()
{
    const auto open = classify({ { "highway", "residential" } });
    check((open.access & map_rules::kAccessMotorcar) != 0, "a residential road is open to cars");

    const auto priv = classify({ { "highway", "residential" }, { "access", "private" } });
    check((priv.access & map_rules::kAccessMotorcar) == 0, "access=private closes it to cars");
    check((priv.access & map_rules::kAccessEmergency) != 0, "but not to emergency vehicles");
    check(priv.drawn(), "and it is still DRAWN -- a private road is on the map");

    const auto noCar = classify({ { "highway", "residential" }, { "motor_vehicle", "no" } });
    check((noCar.access & map_rules::kAccessMotorcar) == 0, "motor_vehicle=no closes it to cars");
    check((noCar.access & map_rules::kAccessFoot) != 0, "and leaves it open on foot");
}

void test_oneway_reverse_is_not_read_as_forward()
{
    // oneway=-1 means the way is one-way AGAINST its own direction. Reading it
    // as plain oneway sends every route down these backwards -- and they are
    // usually slip roads, so the result is short, plausible and illegal.
    const auto forward = classify({ { "highway", "primary" }, { "oneway", "yes" } });
    check(forward.onewayForward && !forward.onewayBackward, "oneway=yes is forward-only");

    const auto reverse = classify({ { "highway", "primary" }, { "oneway", "-1" } });
    check(reverse.onewayBackward && !reverse.onewayForward, "oneway=-1 is backward-only");

    const auto both = classify({ { "highway", "primary" } });
    check(!both.onewayForward && !both.onewayBackward, "and an untagged primary is two-way");
}

void test_a_roundabout_is_oneway_without_saying_so()
{
    const auto result = classify({ { "highway", "tertiary" }, { "junction", "roundabout" } });
    check(result.onewayForward, "a roundabout is one-way even when untagged");
}

void test_mph_is_not_read_as_kph()
{
    // Almost every US maxspeed carries "mph". Reading one as km/h is a 1.6x
    // error that looks entirely plausible on a dial: the driver is told the
    // freeway limit is 40.
    check(map_rules::parseMaxspeed("65 mph").kph == 105, "65 mph is 105 km/h");
    check(map_rules::parseMaxspeed("65mph").kph == 105, "with or without the space");
    check(map_rules::parseMaxspeed("50").kph == 50, "a bare number is km/h");
    check(map_rules::parseMaxspeed("50 km/h").kph == 50, "and so is an explicit one");
    check(!map_rules::parseMaxspeed("none").valid, "'none' is not a number");
    check(!map_rules::parseMaxspeed("walk").valid, "'walk' is not a number");
    check(!map_rules::parseMaxspeed("").valid, "nor is nothing");
    check(!map_rules::parseMaxspeed("50 furlongs").valid, "and an unknown unit is refused");
    check(!map_rules::parseMaxspeed("900").valid, "as is an implausible number");
}

void test_posted_and_free_flow_are_different_numbers()
{
    // The distinction the horizon schema is built around: posted is what a sign
    // says and may be absent, free-flow is always defined and must never be
    // shown to a driver.
    const auto signed_ = classify({ { "highway", "residential" }, { "maxspeed", "25 mph" } });
    check(signed_.hasPosted, "a tagged limit is posted");
    check(signed_.postedSpeedKph == 40, "at its converted value");
    check(signed_.postedSource == map_rules::SpeedSource::Sign, "sourced from a sign");
    check(signed_.freeFlowSpeedKph == 40, "and the router uses the same number");

    const auto untagged = classify({ { "highway", "residential" } });
    check(!untagged.hasPosted, "an untagged road has NO posted limit");
    check(untagged.freeFlowSpeedKph > 0, "but still has a free-flow speed to cost with");
    check(untagged.postedSource == map_rules::SpeedSource::ImplicitClass,
          "and says how weak that guess is");
}

void test_a_conditional_limit_is_flagged_rather_than_evaluated()
{
    const auto result = classify({ { "highway", "primary" },
                                   { "maxspeed", "35 mph" },
                                   { "maxspeed:conditional", "25 @ (Mo-Fr 07:00-09:00)" } });
    check(result.hasPosted, "the unconditional limit is still posted");
    check(result.postedSource == map_rules::SpeedSource::ConditionalIgnored,
          "but flagged as possibly wrong right now");
}

void test_an_unknown_highway_type_is_drawn_but_not_routed()
{
    // Guessing that an unknown road type is driveable is how a router ends up
    // on a construction site.
    const auto result = classify({ { "highway", "some_future_thing" } });
    check(result.drawn(), "an unrecognised highway is still drawn");
    check(!result.routable(), "and is NOT routable");
}

void test_areas_classify_only_when_closed()
{
    const Pairs building { { "building", "yes" } };

    const auto asArea = classify(building, true);
    check(asArea.renderClass == map_rules::RenderClass::Building, "a closed building is a building");
    check(asArea.isArea, "and is marked as an area");

    const auto asLine = classify(building, false);
    check(asLine.renderClass == map_rules::RenderClass::None,
          "and an unclosed one is not drawn as a building");
}

void test_water_and_landuse()
{
    const auto water = classify({ { "natural", "water" } }, true);
    check(water.renderClass == map_rules::RenderClass::Water, "natural=water is water");
    check(water.minZoom <= 8, "and is drawn well before z14");

    const auto park = classify({ { "leisure", "park" } }, true);
    check(park.renderClass == map_rules::RenderClass::Landcover, "a park is landcover");

    const auto river = classify({ { "waterway", "river" } });
    check(river.renderClass == map_rules::RenderClass::Waterway, "a river is a waterway");
    check(!river.routable(), "and is not routable");
}

void test_a_ferry_is_routable_without_being_a_highway()
{
    const auto result = classify({ { "route", "ferry" } });
    check(result.routeClass == map_rules::RouteClass::Ferry, "a ferry route is routable");
    check((result.access & map_rules::kAccessMotorcar) != 0, "by car");
    check(result.drawn(), "and is drawn");
}

void test_bridge_tunnel_and_layer_are_carried()
{
    const auto result = classify(
        { { "highway", "motorway" }, { "bridge", "yes" }, { "layer", "2" }, { "lanes", "4" } });
    check(result.isBridge, "a bridge is flagged");
    check(!result.isTunnel, "and is not a tunnel");
    check(result.layer == 2, "with its layer");
    check(result.laneCount == 4, "and its lane count, which guidance will want");

    const auto negative = classify({ { "highway", "primary" }, { "tunnel", "yes" }, { "layer", "-1" } });
    check(negative.isTunnel, "a tunnel is flagged");
    check(negative.layer == -1, "with a negative layer");
}

void test_nothing_at_all_classifies_as_nothing()
{
    const auto result = classify({ { "note", "hello" } });
    check(!result.drawn(), "an entity with no meaningful tags is not drawn");
    check(!result.routable(), "and not routable");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_motorway_is_drawn_and_routable();
    test_a_footway_is_drawn_and_not_driveable();
    test_a_service_road_is_driveable_before_it_is_drawn();
    test_steps_are_walkable_but_not_cyclable();
    test_access_tags_close_a_road();
    test_oneway_reverse_is_not_read_as_forward();
    test_a_roundabout_is_oneway_without_saying_so();
    test_mph_is_not_read_as_kph();
    test_posted_and_free_flow_are_different_numbers();
    test_a_conditional_limit_is_flagged_rather_than_evaluated();
    test_an_unknown_highway_type_is_drawn_but_not_routed();
    test_areas_classify_only_when_closed();
    test_water_and_landuse();
    test_a_ferry_is_routable_without_being_a_highway();
    test_bridge_tunnel_and_layer_are_carried();
    test_nothing_at_all_classifies_as_nothing();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all classification checks passed");
    return 0;
}
