// SPDX-License-Identifier: GPL-3.0-or-later
//
// One set of rules for what a piece of OSM data IS.
//
// THIS LIBRARY IS THE REASON THE EXTRACTOR IS OURS. The tiler and the graph
// builder both call classify(); neither has rules of its own. Two independent
// rule sets -- tilemaker's Lua and a graph builder's C++ -- eventually disagree,
// and the symptom is "the route goes down a road that isn't drawn on the map",
// which costs a day before anyone suspects the cause.
//
// classify() RETURNS A STRUCT, NOT ONE ENUM. The sharing is not symmetric: the
// tiler is zoom-dependent and works on simplified geometry, the graph is
// neither, and highway=footway is drawn but only routable under a pedestrian
// profile. A single enum forces render-only and access-only vocabularies into
// one namespace, and you end up with two tables again -- the exact thing this
// exists to prevent. One tag parse, many answers.
#ifndef MAP_RULES_CLASSIFICATION_H
#define MAP_RULES_CLASSIFICATION_H

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace map_rules
{

// How a thing is DRAWN.
//
// Deliberately short and closed. Every value is switched in the tessellator,
// the style struct, the tiler and the label ranker, so under -Wswitch-enum
// adding one is a permanent four-file edit. That is the trade this tree wants
// for a display vocabulary; the long tail lives in the bitmask fields below and
// in the raw tag carried alongside.
enum class RenderClass : std::uint8_t
{
    None,
    Motorway,
    Trunk,
    Primary,
    Secondary,
    Tertiary,
    // Residential and unclassified, which are the same thing to a driver.
    Minor,
    Service,
    Track,
    Path,
    Pedestrian,
    Rail,
    Ferry,
    Waterway,
    Water,
    Building,
    Landuse,
    Landcover,
    Boundary,
    // A named point: a city, a town, a neighbourhood. The only class that is
    // never a line or an area, and the only one carried by a NODE rather than
    // a way -- which is why classifyPlace() is a separate entry point.
    Place,
};

// What kind of place, which is what a label renderer sorts and sizes by.
//
// Ordered from largest to smallest deliberately: the ordinal IS the priority,
// so a label placer that runs out of room drops the neighbourhood before the
// city without needing a second table.
enum class PlaceKind : std::uint8_t
{
    None,
    Country,
    State,
    City,
    Town,
    Village,
    Suburb,
    Neighbourhood,
    Locality,
};

// How a thing is ROUTED. Not the same vocabulary: there is no Building here and
// no Landcover, and Path means something a pedestrian profile can use.
enum class RouteClass : std::uint8_t
{
    // Not routable at all, for anyone.
    None,
    Motorway,
    Trunk,
    Primary,
    Secondary,
    Tertiary,
    Minor,
    Service,
    Track,
    Path,
    Pedestrian,
    Ferry,
};

// Who may use it. A bitmask rather than an enum: access is genuinely a set, and
// the long tail (agricultural, forestry, delivery) collapses into these without
// growing a switched vocabulary.
using AccessMask = std::uint16_t;

inline constexpr AccessMask kAccessFoot = 1u << 0;
inline constexpr AccessMask kAccessBicycle = 1u << 1;
inline constexpr AccessMask kAccessMotorcar = 1u << 2;
inline constexpr AccessMask kAccessHgv = 1u << 3;
inline constexpr AccessMask kAccessPsv = 1u << 4;
inline constexpr AccessMask kAccessEmergency = 1u << 5;

// Where a speed limit came from. Mirrors MapSpeedSource in map_common.capnp,
// deliberately: this is the value that ends up on the bus.
enum class SpeedSource : std::uint8_t
{
    Unknown,
    // An explicit maxspeed tag. The ONLY value that may be shown to a driver as
    // a posted limit.
    Sign,
    ImplicitUrban,
    ImplicitRural,
    // Inferred from the road class alone -- the weakest guess we make.
    ImplicitClass,
    // A maxspeed:conditional exists and was not evaluated, so the posted value
    // is the unconditional one and may be wrong right now.
    ConditionalIgnored,
};

// Everything one tag parse yields.
struct RoadClassification
{
    RenderClass renderClass { RenderClass::None };
    // Lowest zoom at which the tiler should emit this. 255 means never.
    std::uint8_t minZoom { 255 };
    // Lower sorts first when labels collide. 255 means do not label.
    std::uint8_t labelRank { 255 };

    RouteClass routeClass { RouteClass::None };
    AccessMask access { 0 };

    // Direction of travel. Both false is two-way; both true is a way tagged
    // oneway in a way we could not read, and is treated as closed.
    bool onewayForward { false };
    bool onewayBackward { false };

    // THE TWO SPEEDS, and they are different numbers.
    //
    // `posted` is what a sign says and is frequently ABSENT -- roughly half of
    // highway=residential has no maxspeed. A display must show nothing rather
    // than a default, which is why presence is a flag and not a sentinel: zero
    // is a legal limit in exactly the places (a barrier, a gate) where getting
    // it wrong matters.
    //
    // `freeFlow` is always defined, because a router cannot cost an edge
    // without one. NEVER show it to a driver.
    bool hasPosted { false };
    std::uint16_t postedSpeedKph { 0 };
    SpeedSource postedSource { SpeedSource::Unknown };
    std::uint16_t freeFlowSpeedKph { 0 };

    // Geometry hints the tiler needs and the graph does not.
    bool isArea { false };
    bool isBridge { false };
    bool isTunnel { false };
    std::int8_t layer { 0 };

    std::uint8_t laneCount { 0 };

    // The `class` a tile carries for anything that is not a road: "lake",
    // "grass", "residential" and so on. A pointer into static storage rather
    // than a string, because this struct is filled nine million times per build
    // and the vocabulary is closed.
    //
    // Roads get theirs from roadClassFor(renderClass) instead -- the render
    // class IS the road class, and duplicating it here would be two places to
    // disagree.
    const char* className { "" };

    bool drawn() const { return renderClass != RenderClass::None; }
    bool routable() const { return routeClass != RouteClass::None && access != 0; }
};

// A borrowed view of one entity's tags.
//
// A span of pairs rather than a map: an entity has a handful of tags, a linear
// scan beats a hash for that, and building a map per entity is 9 million
// allocations on a SoCal extract alone.
struct TagView
{
    std::span<const std::pair<std::string_view, std::string_view>> pairs;

    std::optional<std::string_view> get(std::string_view key) const
    {
        for (const auto& [k, v] : pairs)
        {
            if (k == key)
            {
                return v;
            }
        }
        return std::nullopt;
    }

    bool has(std::string_view key) const { return get(key).has_value(); }

    bool is(std::string_view key, std::string_view value) const
    {
        auto found = get(key);
        return found.has_value() && *found == value;
    }
};

// Whether the entity is a closed area rather than a line. The caller knows
// whether the geometry closes; the tags decide the rest.
struct Shape
{
    bool closed { false };
};

RoadClassification classify(const TagView& tags, const Shape& shape);

// A NODE's classification: the label layer.
//
// Separate from classify() rather than folded into it, because the inputs and
// the outputs share nothing. A node has no shape, no direction, no speed and no
// access; what it has is a kind and a population, and what the tiler wants back
// is a zoom and a rank. Forcing that through RoadClassification would mean a
// struct where two thirds of the fields are meaningless for two thirds of the
// callers, which is how a classifier stops being readable.
//
// Population is used where it exists: OSM's place=city/town boundary is drawn
// by local convention and varies by an order of magnitude between countries,
// so a town of 400 000 people should appear before a city of 3 000.
struct PlaceClassification
{
    PlaceKind kind { PlaceKind::None };
    std::uint8_t minZoom { 255 };
    // Lower sorts first when labels collide.
    std::uint8_t labelRank { 255 };
    std::uint32_t population { 0 };

    bool drawn() const { return kind != PlaceKind::None; }
};

PlaceClassification classifyPlace(const TagView& tags);

// Parsing a maxspeed value. Exposed because it is worth testing on its own:
// the unit suffix is optional, "mph" is common in the US and nowhere else, and
// getting it wrong is a 1.6x error that looks entirely plausible.
struct ParsedSpeed
{
    bool valid { false };
    std::uint16_t kph { 0 };
};

ParsedSpeed parseMaxspeed(std::string_view value);

// Whether this is a ROAD -- something a vehicle or a pedestrian travels along,
// as opposed to a railway, a river, a border or a shape.
//
// A predicate rather than a range check on the enum, because the enumerators
// are grouped by accident of writing order and nothing stops the next one being
// inserted in the middle. The switch is exhaustive, so adding a class forces a
// decision here rather than silently answering false.
bool isRoad(RenderClass value);

const char* to_string(RenderClass value);
const char* to_string(RouteClass value);
const char* to_string(PlaceKind value);

} // namespace map_rules

#endif // MAP_RULES_CLASSIFICATION_H
