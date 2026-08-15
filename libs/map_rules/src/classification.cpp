// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_rules/classification.h"

#include <array>
#include <charconv>

namespace map_rules
{
namespace
{

struct HighwayRule
{
    std::string_view tag;
    RenderClass render;
    std::uint8_t minZoom;
    std::uint8_t labelRank;
    RouteClass route;
    AccessMask access;
    // Free-flow speed when nothing else says. US defaults, in km/h -- this is
    // an American vehicle and the numbers are the mph limits it will meet.
    std::uint16_t freeFlowKph;
};

constexpr AccessMask kVehicle =
    kAccessMotorcar | kAccessHgv | kAccessPsv | kAccessEmergency | kAccessBicycle | kAccessFoot;
constexpr AccessMask kMotorOnly = kAccessMotorcar | kAccessHgv | kAccessPsv | kAccessEmergency;
constexpr AccessMask kFootOnly = kAccessFoot;
constexpr AccessMask kFootBike = kAccessFoot | kAccessBicycle;

// The table. Ordered by nothing in particular; looked up linearly, which is
// fine for twenty-odd entries against a tag we already have in a register.
//
// Every row is a deliberate statement about BOTH sides. Where the drawn answer
// and the routable answer differ, that difference is the point:
//   - motorway_link is drawn as its parent class but routes as one.
//   - footway/steps are drawn and are NOT routable by car.
//   - service is routable by car and not drawn until z14, so a route can use a
//     road the map does not show at z12. That is intended and is why the
//     minZoom lives here rather than in the tiler.
constexpr std::array<HighwayRule, 24> kHighways { {
    { "motorway", RenderClass::Motorway, 4, 0, RouteClass::Motorway, kMotorOnly, 105 },
    { "motorway_link", RenderClass::Motorway, 10, 6, RouteClass::Motorway, kMotorOnly, 64 },
    { "trunk", RenderClass::Trunk, 5, 1, RouteClass::Trunk, kMotorOnly, 89 },
    { "trunk_link", RenderClass::Trunk, 10, 6, RouteClass::Trunk, kMotorOnly, 56 },
    { "primary", RenderClass::Primary, 7, 2, RouteClass::Primary, kVehicle, 89 },
    { "primary_link", RenderClass::Primary, 11, 6, RouteClass::Primary, kVehicle, 48 },
    { "secondary", RenderClass::Secondary, 9, 3, RouteClass::Secondary, kVehicle, 72 },
    { "secondary_link", RenderClass::Secondary, 12, 6, RouteClass::Secondary, kVehicle, 40 },
    { "tertiary", RenderClass::Tertiary, 10, 4, RouteClass::Tertiary, kVehicle, 56 },
    { "tertiary_link", RenderClass::Tertiary, 13, 6, RouteClass::Tertiary, kVehicle, 40 },
    { "residential", RenderClass::Minor, 12, 5, RouteClass::Minor, kVehicle, 40 },
    { "unclassified", RenderClass::Minor, 12, 5, RouteClass::Minor, kVehicle, 40 },
    { "living_street", RenderClass::Minor, 13, 5, RouteClass::Minor, kVehicle, 20 },
    { "road", RenderClass::Minor, 13, 6, RouteClass::Minor, kVehicle, 40 },
    { "service", RenderClass::Service, 14, 8, RouteClass::Service, kVehicle, 16 },
    { "track", RenderClass::Track, 14, 9, RouteClass::Track, kVehicle, 16 },
    { "pedestrian", RenderClass::Pedestrian, 13, 7, RouteClass::Pedestrian, kFootBike, 5 },
    { "footway", RenderClass::Path, 14, 10, RouteClass::Path, kFootOnly, 5 },
    { "path", RenderClass::Path, 14, 10, RouteClass::Path, kFootBike, 5 },
    { "cycleway", RenderClass::Path, 13, 10, RouteClass::Path, kFootBike, 16 },
    { "bridleway", RenderClass::Path, 14, 10, RouteClass::Path, kFootOnly, 5 },
    // Steps are drawn and are routable on foot, but a router must never take
    // them for a bicycle -- hence foot only rather than kFootBike.
    { "steps", RenderClass::Path, 14, 10, RouteClass::Path, kFootOnly, 2 },
    { "corridor", RenderClass::Path, 14, 255, RouteClass::Path, kFootOnly, 5 },
    { "busway", RenderClass::Secondary, 12, 6, RouteClass::Secondary, kAccessPsv | kAccessEmergency,
      48 },
} };

const HighwayRule* highwayRule(std::string_view value)
{
    for (const HighwayRule& rule : kHighways)
    {
        if (rule.tag == value)
        {
            return &rule;
        }
    }
    return nullptr;
}

bool isNo(std::string_view value)
{
    return value == "no" || value == "private" || value == "false";
}

bool isYes(std::string_view value)
{
    return value == "yes" || value == "true" || value == "1" || value == "designated" ||
           value == "permissive" || value == "destination";
}

// Apply access=*, motor_vehicle=*, foot=* and friends onto a starting mask.
AccessMask applyAccess(const TagView& tags, AccessMask mask)
{
    if (auto general = tags.get("access"); general.has_value() && isNo(*general))
    {
        // access=private closes everything except emergency, which is the
        // convention every router follows.
        mask &= kAccessEmergency;
    }

    const auto apply = [&](std::string_view key, AccessMask bits) {
        auto value = tags.get(key);
        if (!value.has_value())
        {
            return;
        }
        if (isNo(*value))
        {
            mask &= static_cast<AccessMask>(~bits);
        }
        else if (isYes(*value))
        {
            mask |= bits;
        }
    };

    apply("motor_vehicle", kAccessMotorcar | kAccessHgv);
    apply("motorcar", kAccessMotorcar);
    apply("hgv", kAccessHgv);
    apply("psv", kAccessPsv);
    apply("bus", kAccessPsv);
    apply("foot", kAccessFoot);
    apply("bicycle", kAccessBicycle);
    apply("emergency", kAccessEmergency);

    return mask;
}

void applyOneway(const TagView& tags, RoadClassification& out)
{
    auto oneway = tags.get("oneway");
    if (oneway.has_value())
    {
        if (*oneway == "yes" || *oneway == "1" || *oneway == "true")
        {
            out.onewayForward = true;
        }
        else if (*oneway == "-1" || *oneway == "reverse")
        {
            // The reversed form. Reading it as plain oneway sends every route
            // down these the wrong way -- and they are usually slip roads, so
            // the route is short, plausible and illegal.
            out.onewayBackward = true;
        }
        return;
    }

    // A motorway carriageway is one-way by default and very often untagged;
    // a roundabout likewise. Missing this makes a divided highway bidirectional,
    // which lets a router U-turn across the median.
    if (tags.is("junction", "roundabout") || tags.is("junction", "circular"))
    {
        out.onewayForward = true;
        return;
    }
    if (out.routeClass == RouteClass::Motorway && !tags.is("highway", "motorway_link"))
    {
        out.onewayForward = true;
    }
}

void applySpeed(const TagView& tags, RoadClassification& out, std::uint16_t classDefault)
{
    out.freeFlowSpeedKph = classDefault;

    if (auto maxspeed = tags.get("maxspeed"); maxspeed.has_value())
    {
        const ParsedSpeed parsed = parseMaxspeed(*maxspeed);
        if (parsed.valid)
        {
            out.hasPosted = true;
            out.postedSpeedKph = parsed.kph;
            out.postedSource = tags.has("maxspeed:conditional") ? SpeedSource::ConditionalIgnored
                                                                : SpeedSource::Sign;
            out.freeFlowSpeedKph = parsed.kph;
            return;
        }
        // "none", "walk", "signals" and friends: a real tag we cannot turn into
        // a number. Not posted, and the class default stands.
        if (*maxspeed == "walk")
        {
            out.freeFlowSpeedKph = 7;
        }
    }

    // No sign. The source says how weak the guess is, so a display can refuse
    // to show it while a router still has something to cost with.
    out.postedSource = SpeedSource::ImplicitClass;
}

// GROUND COVER, which OSM spells across four different keys.
//
// `natural=wood` and `landuse=forest` are the same trees; `natural=grassland`,
// `landuse=grass` and `leisure=park` are all green. A style wants one word for
// each, so the mapping collapses the synonyms rather than passing the raw tag
// through -- otherwise every style has to carry this table instead.
//
// Returns "" for "this is not ground cover", so the caller can fall through to
// the landuse and leisure rules below.
const char* landcoverClass(const TagView& tags)
{
    if (const auto natural = tags.get("natural"); natural.has_value())
    {
        const auto value = *natural;
        if (value == "wood") { return "wood"; }
        if (value == "scrub" || value == "heath" || value == "grassland") { return "grass"; }
        if (value == "sand" || value == "beach" || value == "dune") { return "sand"; }
        if (value == "wetland" || value == "marsh" || value == "mud") { return "wetland"; }
        if (value == "bare_rock" || value == "scree" || value == "shingle" ||
            value == "cliff")
        {
            return "rock";
        }
        if (value == "glacier") { return "ice"; }
    }
    if (const auto landuse = tags.get("landuse"); landuse.has_value())
    {
        const auto value = *landuse;
        if (value == "forest") { return "wood"; }
        if (value == "grass" || value == "meadow" || value == "village_green" ||
            value == "recreation_ground" || value == "allotments")
        {
            return "grass";
        }
        if (value == "farmland" || value == "farm" || value == "orchard" ||
            value == "vineyard" || value == "plant_nursery")
        {
            return "farmland";
        }
        if (value == "salt_pond") { return "wetland"; }
    }
    if (const auto leisure = tags.get("leisure"); leisure.has_value())
    {
        const auto value = *leisure;
        // NOT nature_reserve, which is a DESIGNATION rather than a cover: a
        // reserve is forest, rock, scrub and water at once, and painting the
        // whole outline green states something about the ground that is not
        // true. It belongs to the park layer, which classifyPark() answers for.
        if (value == "park" || value == "garden" || value == "golf_course")
        {
            return "grass";
        }
    }
    if (tags.is("wetland", "yes") || tags.has("wetland"))
    {
        return "wetland";
    }
    return "";
}

// WHAT A PLACE IS FOR: residential, industrial, a school, a cemetery.
//
// Distinct from ground cover: landcover says what is on the ground, landuse says
// what people do there, and a style colours them differently. Unrecognised
// values pass through unchanged rather than being dropped -- the value IS the
// class in OSM's vocabulary for most of this key, so a pass-through is right far
// more often than a discard, and a style that does not know a word simply does
// not draw it.
const char* landuseClass(std::string_view value)
{
    if (value == "residential") { return "residential"; }
    if (value == "commercial") { return "commercial"; }
    if (value == "industrial" || value == "port" || value == "depot") { return "industrial"; }
    if (value == "retail") { return "retail"; }
    if (value == "railway") { return "railway"; }
    if (value == "cemetery" || value == "grave_yard") { return "cemetery"; }
    if (value == "military") { return "military"; }
    if (value == "quarry" || value == "landfill") { return "quarry"; }
    if (value == "construction" || value == "brownfield" || value == "greenfield")
    {
        return "construction";
    }
    if (value == "school" || value == "education") { return "school"; }
    return "other";
}

// Leisure as land USE rather than land cover -- the built facilities. The green
// values (park, garden, golf course) never reach here; landcoverClass() claims
// them first.
const char* leisureClass(std::string_view value)
{
    // Empty means "not land use at all" -- a nature reserve is a designation
    // over terrain and lives in the park layer instead. Filling it here would
    // draw it twice, once as a reserve and once as ground.
    if (value == "nature_reserve")
    {
        return "";
    }
    if (value == "pitch" || value == "sports_centre" || value == "sports_hall")
    {
        return "pitch";
    }
    if (value == "stadium") { return "stadium"; }
    if (value == "track" || value == "horse_riding") { return "track"; }
    if (value == "playground") { return "playground"; }
    if (value == "marina") { return "marina"; }
    return "other";
}

// A waterway as a line.
//
// Every branch returns a STRING LITERAL, never a pointer into the tag value.
// RoadClassification::className outlives the TagView it was built from -- the
// tag value is a string_view into a decompressed PBF block that the caller is
// free to drop -- and a string_view is not null-terminated anyway, so anything
// derived from the tag text would be a dangling read of whatever followed it in
// the block. Unrecognised values collapse to "stream" rather than being
// forwarded.
const char* waterwayClass(std::string_view value)
{
    if (value == "river" || value == "riverbank") { return "river"; }
    if (value == "canal") { return "canal"; }
    if (value == "drain") { return "drain"; }
    if (value == "ditch") { return "ditch"; }
    if (value == "dock") { return "dock"; }
    return "stream";
}

// An area rule carries its own zoom and label rank.
//
// Returning the class alone would force a second switch over RenderClass at the
// call site, and -Wswitch-enum would then demand all nineteen enumerators be
// named to answer a question about four of them. Carrying the answer with the
// class keeps the exhaustiveness rule useful where it matters -- the tessellator
// and the tiler -- rather than noisy here.
struct AreaRule
{
    RenderClass render { RenderClass::None };
    std::uint8_t minZoom { 255 };
    std::uint8_t labelRank { 255 };
    const char* className { "" };
};

// What a closed way, or an assembled multipolygon, IS.
//
// Order matters here and it is not the obvious one. WATER IS TESTED FIRST,
// before landuse and leisure, because the tags overlap: a swimming pool is
// `leisure=swimming_pool` and a settling pond is `landuse=basin`, and a rule
// that reached "leisure" or "landuse" first would file both as ground cover.
// In suburban Southern California that single ordering decision accounts for
// tens of thousands of features -- the region is full of pools.
AreaRule areaRule(const TagView& tags)
{
    if (tags.has("building") && !tags.is("building", "no"))
    {
        return { RenderClass::Building, 13, 255, "building" };
    }

    // Water, in every spelling OSM uses for it.
    if (tags.is("natural", "water") || tags.is("landuse", "reservoir") ||
        tags.is("landuse", "basin") || tags.is("leisure", "swimming_pool"))
    {
        // A `water=river` on an area means a wide river drawn as a shape rather
        // than a line, which a style may want to treat as moving water.
        const auto water = tags.get("water");
        const bool river = water.has_value() && (*water == "river" || *water == "canal" ||
                                                 *water == "stream" || *water == "ditch" ||
                                                 *water == "drain");
        return { RenderClass::Water, 6, 11, river ? "river" : "lake" };
    }
    // A waterway mapped as an AREA -- a riverbank, a dock, a wide canal. As a
    // line it is a waterway; closed, it is water with a shape.
    if (const auto waterway = tags.get("waterway"); waterway.has_value())
    {
        if (*waterway == "riverbank" || *waterway == "dock" || *waterway == "river" ||
            *waterway == "canal" || *waterway == "stream" || *waterway == "drain" ||
            *waterway == "ditch")
        {
            return { RenderClass::Water, 6, 11, "river" };
        }
    }
    if (tags.has("water"))
    {
        return { RenderClass::Water, 6, 11, "lake" };
    }

    // Ground cover, whatever key it is spelled under. Checked before landuse
    // because several of these values live on the `landuse` key.
    if (const char* cover = landcoverClass(tags); cover[0] != '\0')
    {
        return { RenderClass::Landcover, 8, 255, cover };
    }
    if (const auto landuse = tags.get("landuse"); landuse.has_value())
    {
        return { RenderClass::Landuse, 8, 255, landuseClass(*landuse) };
    }
    if (const auto leisure = tags.get("leisure"); leisure.has_value())
    {
        if (const char* leisureName = leisureClass(*leisure); leisureName[0] != '\0')
        {
            return { RenderClass::Landuse, 8, 255, leisureName };
        }
    }
    return {};
}

} // namespace

ParsedSpeed parseMaxspeed(std::string_view value)
{
    ParsedSpeed out;

    std::size_t digits = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9')
    {
        ++digits;
    }
    if (digits == 0)
    {
        return out;
    }

    unsigned number = 0;
    const auto [ptr, ec] =
        std::from_chars(value.data(), value.data() + digits, number);
    (void)ptr;
    if (ec != std::errc {})
    {
        return out;
    }

    std::string_view suffix = value.substr(digits);
    while (!suffix.empty() && suffix.front() == ' ')
    {
        suffix.remove_prefix(1);
    }

    // The unit suffix is optional and defaults to km/h. In the US almost every
    // maxspeed carries "mph", and reading one as km/h is a 1.6x error that
    // looks entirely plausible on a dial -- 65 becomes 65, and the driver is
    // told the freeway limit is 40.
    if (suffix.starts_with("mph"))
    {
        number = static_cast<unsigned>((number * 1609 + 500) / 1000);
    }
    else if (suffix.starts_with("knots"))
    {
        number = static_cast<unsigned>((number * 1852 + 500) / 1000);
    }
    else if (!suffix.empty() && !suffix.starts_with("km/h") && !suffix.starts_with("kph"))
    {
        // A unit we do not know. Refused rather than guessed.
        return out;
    }

    if (number == 0 || number > 400)
    {
        return out;
    }

    out.valid = true;
    out.kph = static_cast<std::uint16_t>(number);
    return out;
}

RoadClassification classify(const TagView& tags, const Shape& shape)
{
    RoadClassification out;

    if (auto highway = tags.get("highway"); highway.has_value())
    {
        if (const HighwayRule* rule = highwayRule(*highway); rule != nullptr)
        {
            out.renderClass = rule->render;
            out.minZoom = rule->minZoom;
            out.labelRank = rule->labelRank;
            out.routeClass = rule->route;
            out.access = applyAccess(tags, rule->access);
            applyOneway(tags, out);
            applySpeed(tags, out, rule->freeFlowKph);

            out.isBridge = tags.has("bridge") && !tags.is("bridge", "no");
            out.isTunnel = tags.has("tunnel") && !tags.is("tunnel", "no");
            if (auto layer = tags.get("layer"); layer.has_value())
            {
                int parsed = 0;
                const auto [ptr, ec] =
                    std::from_chars(layer->data(), layer->data() + layer->size(), parsed);
                (void)ptr;
                if (ec == std::errc {} && parsed >= -128 && parsed <= 127)
                {
                    out.layer = static_cast<std::int8_t>(parsed);
                }
            }
            if (auto lanes = tags.get("lanes"); lanes.has_value())
            {
                unsigned parsed = 0;
                const auto [ptr, ec] =
                    std::from_chars(lanes->data(), lanes->data() + lanes->size(), parsed);
                (void)ptr;
                if (ec == std::errc {} && parsed <= 32)
                {
                    out.laneCount = static_cast<std::uint8_t>(parsed);
                }
            }
            return out;
        }
        // A highway value this build has no word for. Drawn as a minor road so
        // it does not vanish from the map, and NOT routable -- guessing that an
        // unknown road type is driveable is how a router ends up on a
        // construction site.
        out.renderClass = RenderClass::Minor;
        out.minZoom = 14;
        out.labelRank = 12;
        return out;
    }

    // A ferry is a routable line that is not a highway.
    if (tags.is("route", "ferry"))
    {
        out.renderClass = RenderClass::Ferry;
        out.className = "ferry";
        out.minZoom = 8;
        out.labelRank = 7;
        out.routeClass = RouteClass::Ferry;
        out.access = applyAccess(tags, kVehicle);
        applySpeed(tags, out, 25);
        return out;
    }

    if (const auto railway = tags.get("railway");
        railway.has_value() && *railway != "abandoned")
    {
        out.renderClass = RenderClass::Rail;
        out.minZoom = 10;
        out.labelRank = 255;
        // "rail" is the heavy stuff, "transit" is everything a passenger boards
        // in a city. Two words rather than the raw tag, because a style draws
        // exactly these two weights.
        out.className = (*railway == "subway" || *railway == "light_rail" ||
                         *railway == "tram" || *railway == "monorail" ||
                         *railway == "funicular")
                            ? "transit"
                            : "rail";
        return out;
    }

    // A waterway as a LINE. The area spellings are claimed by areaRule() below,
    // which runs only when the geometry closes.
    if (const auto waterway = tags.get("waterway"); waterway.has_value())
    {
        if (!shape.closed)
        {
            out.renderClass = RenderClass::Waterway;
            out.minZoom = 10;
            out.labelRank = 11;
            out.className = waterwayClass(*waterway);
            return out;
        }
    }

    if (tags.is("boundary", "administrative"))
    {
        out.renderClass = RenderClass::Boundary;
        out.minZoom = 4;
        out.labelRank = 255;
        out.className = "administrative";
        return out;
    }

    if (shape.closed)
    {
        const AreaRule area = areaRule(tags);
        if (area.render != RenderClass::None)
        {
            out.renderClass = area.render;
            out.minZoom = area.minZoom;
            out.labelRank = area.labelRank;
            out.className = area.className;
            out.isArea = true;
        }
    }

    return out;
}

PlaceClassification classifyPlace(const TagView& tags)
{
    PlaceClassification out;

    auto place = tags.get("place");
    if (!place)
    {
        return out;
    }

    // A place with no name is a label with nothing to say. It happens -- a
    // boundary node tagged place=suburb and nothing else -- and carrying it
    // costs a point in every tile for a renderer that will skip it.
    if (!tags.has("name"))
    {
        return out;
    }

    struct PlaceRule
    {
        std::string_view value;
        PlaceKind kind;
        std::uint8_t minZoom;
        std::uint8_t labelRank;
    };

    // One table, in the order a label placer should prefer them. minZoom is
    // where the label may FIRST appear, not where it must: a style is free to
    // be stricter, and nothing can draw what the tile does not carry.
    static constexpr std::array<PlaceRule, 11> kRules { {
        { "country", PlaceKind::Country, 2, 0 },
        { "state", PlaceKind::State, 4, 1 },
        { "province", PlaceKind::State, 4, 1 },
        { "city", PlaceKind::City, 6, 2 },
        { "town", PlaceKind::Town, 9, 3 },
        { "village", PlaceKind::Village, 11, 4 },
        { "hamlet", PlaceKind::Village, 13, 5 },
        { "suburb", PlaceKind::Suburb, 11, 6 },
        { "quarter", PlaceKind::Suburb, 12, 6 },
        { "neighbourhood", PlaceKind::Neighbourhood, 13, 7 },
        { "locality", PlaceKind::Locality, 14, 8 },
    } };

    for (const PlaceRule& rule : kRules)
    {
        if (*place == rule.value)
        {
            out.kind = rule.kind;
            out.minZoom = rule.minZoom;
            out.labelRank = rule.labelRank;
            break;
        }
    }

    if (!out.drawn())
    {
        return out;
    }

    // Population, where OSM records it. The city/town line is drawn by local
    // convention and varies by an order of magnitude between countries, so a
    // large town should outrank a small city rather than lose to it on the tag
    // alone.
    if (auto population = tags.get("population"))
    {
        std::uint64_t parsed = 0;
        bool digits = false;
        for (const char c : *population)
        {
            // Thousands separators are common in this field and are not an
            // error; anything else ends the number.
            if (c == ',' || c == ' ' || c == '.')
            {
                continue;
            }
            if (c < '0' || c > '9')
            {
                digits = false;
                break;
            }
            digits = true;
            parsed = parsed * 10 + static_cast<std::uint64_t>(c - '0');
            if (parsed > 100'000'000)
            {
                // Larger than any city on earth: the tag is wrong, and using it
                // would put a village's label above a capital's.
                digits = false;
                break;
            }
        }
        if (digits)
        {
            out.population = static_cast<std::uint32_t>(parsed);

            // A big enough settlement earns the zoom of the tier above it. Only
            // upward: a city tagged with a small population is far more often a
            // stale tag than a genuinely tiny city.
            if (out.population >= 200'000 && out.minZoom > 6)
            {
                out.minZoom = 6;
            }
            else if (out.population >= 50'000 && out.minZoom > 9)
            {
                out.minZoom = 9;
            }
        }
    }

    return out;
}

const char* to_string(PlaceKind value)
{
    switch (value)
    {
        case PlaceKind::None:
            return "none";
        case PlaceKind::Country:
            return "country";
        case PlaceKind::State:
            return "state";
        case PlaceKind::City:
            return "city";
        case PlaceKind::Town:
            return "town";
        case PlaceKind::Village:
            return "village";
        case PlaceKind::Suburb:
            return "suburb";
        case PlaceKind::Neighbourhood:
            return "neighbourhood";
        case PlaceKind::Locality:
            return "locality";
    }
    return "unknown";
}

bool isRoad(RenderClass value)
{
    switch (value)
    {
        case RenderClass::Motorway:
        case RenderClass::Trunk:
        case RenderClass::Primary:
        case RenderClass::Secondary:
        case RenderClass::Tertiary:
        case RenderClass::Minor:
        case RenderClass::Service:
        case RenderClass::Track:
        case RenderClass::Path:
        case RenderClass::Pedestrian:
            return true;
        case RenderClass::None:
        case RenderClass::Rail:
        case RenderClass::Ferry:
        case RenderClass::Waterway:
        case RenderClass::Water:
        case RenderClass::Building:
        case RenderClass::Landuse:
        case RenderClass::Landcover:
        case RenderClass::Boundary:
        case RenderClass::Place:
            return false;
    }
    return false;
}

const char* to_string(RenderClass value)
{
    switch (value)
    {
        case RenderClass::None:
            return "none";
        case RenderClass::Motorway:
            return "motorway";
        case RenderClass::Trunk:
            return "trunk";
        case RenderClass::Primary:
            return "primary";
        case RenderClass::Secondary:
            return "secondary";
        case RenderClass::Tertiary:
            return "tertiary";
        case RenderClass::Minor:
            return "minor";
        case RenderClass::Service:
            return "service";
        case RenderClass::Track:
            return "track";
        case RenderClass::Path:
            return "path";
        case RenderClass::Pedestrian:
            return "pedestrian";
        case RenderClass::Rail:
            return "rail";
        case RenderClass::Ferry:
            return "ferry";
        case RenderClass::Waterway:
            return "waterway";
        case RenderClass::Water:
            return "water";
        case RenderClass::Building:
            return "building";
        case RenderClass::Landuse:
            return "landuse";
        case RenderClass::Landcover:
            return "landcover";
        case RenderClass::Boundary:
            return "boundary";
        case RenderClass::Place:
            return "place";
    }
    return "unknown";
}

const char* to_string(RouteClass value)
{
    switch (value)
    {
        case RouteClass::None:
            return "none";
        case RouteClass::Motorway:
            return "motorway";
        case RouteClass::Trunk:
            return "trunk";
        case RouteClass::Primary:
            return "primary";
        case RouteClass::Secondary:
            return "secondary";
        case RouteClass::Tertiary:
            return "tertiary";
        case RouteClass::Minor:
            return "minor";
        case RouteClass::Service:
            return "service";
        case RouteClass::Track:
            return "track";
        case RouteClass::Path:
            return "path";
        case RouteClass::Pedestrian:
            return "pedestrian";
        case RouteClass::Ferry:
            return "ferry";
    }
    return "unknown";
}

} // namespace map_rules
