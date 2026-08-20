// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_build/tiler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "mvt/encode.h"
#include "mvt/tile.h"

namespace map_build
{
namespace
{

constexpr double kCoordScale = 1e-7;

// Latitude beyond which Mercator's tan() runs away. The same clamp
// dashboard/widgets/map/projection.h applies, and for the same reason: past it
// every subsequent arithmetic yields NaN and the map paints nothing at all,
// silently.
constexpr double kMaxLatitude = 85.0511287798;

double worldX(std::int32_t lon)
{
    return (static_cast<double>(lon) * kCoordScale + 180.0) / 360.0;
}

double worldY(std::int32_t lat)
{
    const double clamped = std::clamp(static_cast<double>(lat) * kCoordScale, -kMaxLatitude,
                                      kMaxLatitude);
    const double radians = clamped * std::numbers::pi / 180.0;
    return (1.0 - std::log(std::tan(radians) + 1.0 / std::cos(radians)) / std::numbers::pi) / 2.0;
}

// Perpendicular distance from p to the line ab, squared, in world units.
double perpendicularSq(double px, double py, double ax, double ay, double bx, double by)
{
    const double dx = bx - ax;
    const double dy = by - ay;
    const double lengthSq = dx * dx + dy * dy;
    if (lengthSq <= 0.0)
    {
        const double ex = px - ax;
        const double ey = py - ay;
        return ex * ex + ey * ey;
    }
    double t = ((px - ax) * dx + (py - ay) * dy) / lengthSq;
    t = std::clamp(t, 0.0, 1.0);
    const double ex = px - (ax + t * dx);
    const double ey = py - (ay + t * dy);
    return ex * ex + ey * ey;
}

// Douglas-Peucker, iterative so a pathological line cannot blow the stack.
//
// Keeps the endpoints and every point further than `tolerance` from the chord
// its neighbours span. A tolerance of about half a tile unit is the sweet spot:
// finer and tiles carry points nobody can see, coarser and roads visibly wander
// off the junctions they are supposed to meet.
void simplify(const std::vector<double>& in, double tolerance, std::vector<double>& out)
{
    const std::size_t count = in.size() / 2;
    out.clear();
    if (count < 3)
    {
        out = in;
        return;
    }

    const double toleranceSq = tolerance * tolerance;
    std::vector<bool> keep(count, false);
    keep.front() = true;
    keep.back() = true;

    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.emplace_back(0, count - 1);

    while (!stack.empty())
    {
        const auto [first, last] = stack.back();
        stack.pop_back();
        if (last <= first + 1)
        {
            continue;
        }

        double worst = 0.0;
        std::size_t worstAt = first;
        for (std::size_t i = first + 1; i < last; ++i)
        {
            const double d = perpendicularSq(in[i * 2], in[i * 2 + 1], in[first * 2],
                                             in[first * 2 + 1], in[last * 2], in[last * 2 + 1]);
            if (d > worst)
            {
                worst = d;
                worstAt = i;
            }
        }

        if (worst > toleranceSq)
        {
            keep[worstAt] = true;
            stack.emplace_back(first, worstAt);
            stack.emplace_back(worstAt, last);
        }
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        if (keep[i])
        {
            out.push_back(in[i * 2]);
            out.push_back(in[i * 2 + 1]);
        }
    }
}

struct Rect
{
    double minX;
    double minY;
    double maxX;
    double maxY;
};

bool inside(double x, double y, const Rect& r, int edge)
{
    switch (edge)
    {
        case 0:
            return x >= r.minX;
        case 1:
            return x <= r.maxX;
        case 2:
            return y >= r.minY;
        default:
            return y <= r.maxY;
    }
}

void intersect(double x1, double y1, double x2, double y2, const Rect& r, int edge, double& outX,
               double& outY)
{
    switch (edge)
    {
        case 0:
            outX = r.minX;
            outY = y1 + (y2 - y1) * (r.minX - x1) / (x2 - x1);
            return;
        case 1:
            outX = r.maxX;
            outY = y1 + (y2 - y1) * (r.maxX - x1) / (x2 - x1);
            return;
        case 2:
            outY = r.minY;
            outX = x1 + (x2 - x1) * (r.minY - y1) / (y2 - y1);
            return;
        default:
            outY = r.maxY;
            outX = x1 + (x2 - x1) * (r.maxY - y1) / (y2 - y1);
            return;
    }
}

// Sutherland-Hodgman, for areas.
//
// Produces a single ring that hugs the clip rectangle where the polygon left
// it, which is what a filled shape needs -- a line clipper would leave the fill
// open and the renderer would close it across the tile.
std::vector<double> clipPolygon(const std::vector<double>& ring, const Rect& r)
{
    std::vector<double> current = ring;
    for (int edge = 0; edge < 4 && !current.empty(); ++edge)
    {
        std::vector<double> next;
        const std::size_t count = current.size() / 2;
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t j = (i + 1) % count;
            const double x1 = current[i * 2];
            const double y1 = current[i * 2 + 1];
            const double x2 = current[j * 2];
            const double y2 = current[j * 2 + 1];

            const bool in1 = inside(x1, y1, r, edge);
            const bool in2 = inside(x2, y2, r, edge);

            if (in1)
            {
                next.push_back(x1);
                next.push_back(y1);
            }
            if (in1 != in2)
            {
                double ix = 0.0;
                double iy = 0.0;
                intersect(x1, y1, x2, y2, r, edge, ix, iy);
                next.push_back(ix);
                next.push_back(iy);
            }
        }
        current.swap(next);
    }
    return current;
}

// Cohen-Sutherland style outcodes, for lines.
int outcode(double x, double y, const Rect& r)
{
    int code = 0;
    if (x < r.minX)
    {
        code |= 1;
    }
    if (x > r.maxX)
    {
        code |= 2;
    }
    if (y < r.minY)
    {
        code |= 4;
    }
    if (y > r.maxY)
    {
        code |= 8;
    }
    return code;
}

// Clip a polyline, producing zero or more PARTS.
//
// Parts, not one line: a road that leaves the tile and comes back is two
// separate pieces of geometry, and joining them would draw a straight line
// across the tile between the two crossings.
void clipLine(const std::vector<double>& line, const Rect& r,
              std::vector<std::vector<double>>& parts)
{
    const std::size_t count = line.size() / 2;
    if (count < 2)
    {
        return;
    }

    std::vector<double> current;
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        double x1 = line[i * 2];
        double y1 = line[i * 2 + 1];
        double x2 = line[(i + 1) * 2];
        double y2 = line[(i + 1) * 2 + 1];

        int code1 = outcode(x1, y1, r);
        int code2 = outcode(x2, y2, r);
        bool accept = false;

        while (true)
        {
            if ((code1 | code2) == 0)
            {
                accept = true;
                break;
            }
            if ((code1 & code2) != 0)
            {
                break;
            }

            const int code = code1 != 0 ? code1 : code2;
            double x = 0.0;
            double y = 0.0;
            if ((code & 8) != 0)
            {
                x = x1 + (x2 - x1) * (r.maxY - y1) / (y2 - y1);
                y = r.maxY;
            }
            else if ((code & 4) != 0)
            {
                x = x1 + (x2 - x1) * (r.minY - y1) / (y2 - y1);
                y = r.minY;
            }
            else if ((code & 2) != 0)
            {
                y = y1 + (y2 - y1) * (r.maxX - x1) / (x2 - x1);
                x = r.maxX;
            }
            else
            {
                y = y1 + (y2 - y1) * (r.minX - x1) / (x2 - x1);
                x = r.minX;
            }

            if (code == code1)
            {
                x1 = x;
                y1 = y;
                code1 = outcode(x1, y1, r);
            }
            else
            {
                x2 = x;
                y2 = y;
                code2 = outcode(x2, y2, r);
            }
        }

        if (!accept)
        {
            // This leg is entirely outside, so whatever part was being built
            // ends here.
            if (current.size() >= 4)
            {
                parts.push_back(current);
            }
            current.clear();
            continue;
        }

        if (current.empty())
        {
            current.push_back(x1);
            current.push_back(y1);
        }
        current.push_back(x2);
        current.push_back(y2);

        // If the leg was cut at its far end, the line left the rectangle and
        // the part is finished.
        if (x2 != line[(i + 1) * 2] || y2 != line[(i + 1) * 2 + 1])
        {
            if (current.size() >= 4)
            {
                parts.push_back(current);
            }
            current.clear();
        }
    }

    if (current.size() >= 4)
    {
        parts.push_back(current);
    }
}

// Collapse lines that share every attribute into one multi-part feature.
//
// WHY IT IS ZOOM-DEPENDENT, and why that is a contract rather than a tuning
// knob. Every drawn feature carries its source way id, which is what lets a
// client recolour the road it is on or draw a route by highlighting features it
// already has. Merging makes that mapping many-to-one and therefore lossy: the
// merged feature is `Jamboree Road`, not any one way. So it happens ONLY below
// the zooms where a client would join back to the graph, and above that the
// per-way identity is preserved exactly.
//
// Below those zooms it is most of what makes a low-zoom tile a usable size: a
// z9 tile over a city holds thousands of separate road ways, each repeating the
// same class/name/ref tags and each paying a feature header, for a picture in
// which they are one continuous line.
//
// Attribute equality only -- endpoints are NOT joined. Joining would need a
// spatial index per tile and would change the drawn geometry; grouping changes
// nothing about what is drawn, only how many features say it.
void mergeLines(mvt::Layer& layer, std::uint8_t zoom, std::uint8_t mergeBelowZoom,
                TileStats& stats)
{
    if (zoom >= mergeBelowZoom || layer.features.size() < 2)
    {
        return;
    }

    // Key on the tag INDICES, which are already interned per layer, so two
    // features with the same attributes have byte-identical tag vectors. No
    // string comparison, and no chance of two spellings of one value being
    // treated as different.
    std::map<std::vector<std::uint32_t>, std::size_t> byTags;
    std::vector<mvt::Feature> merged;
    merged.reserve(layer.features.size());

    for (mvt::Feature& feature : layer.features)
    {
        if (feature.type != mvt::GeomType::LineString)
        {
            merged.push_back(std::move(feature));
            continue;
        }

        auto [entry, inserted] = byTags.try_emplace(feature.tags, merged.size());
        if (inserted)
        {
            // The first of its kind keeps its id only until a second arrives.
            merged.push_back(std::move(feature));
            continue;
        }

        mvt::Feature& target = merged[entry->second];
        // The moment two ways share a feature, the id stops identifying either
        // of them. Dropping it is the honest answer; keeping the first would
        // silently attribute the whole road to one arbitrary way.
        target.hasId = false;
        target.id = 0;
        for (auto& ring : feature.rings)
        {
            target.rings.push_back(std::move(ring));
        }
        ++stats.mergedLines;
    }

    layer.features = std::move(merged);
}

struct TileKey
{
    std::uint32_t x;
    std::uint32_t y;

    friend bool operator==(const TileKey&, const TileKey&) = default;
};

struct TileKeyHash
{
    std::size_t operator()(const TileKey& key) const
    {
        return (static_cast<std::size_t>(key.x) << 32) ^ key.y;
    }
};

// One layer under construction, with its own key/value dictionaries.
struct LayerBuilder
{
    mvt::Layer layer;
    std::unordered_map<std::string, std::uint32_t> keyIndex;
    std::map<std::string, std::uint32_t> valueIndex;

    std::uint32_t key(const std::string& name)
    {
        auto [entry, inserted] = keyIndex.try_emplace(name, static_cast<std::uint32_t>(layer.keys.size()));
        if (inserted)
        {
            layer.keys.push_back(name);
        }
        return entry->second;
    }

    std::uint32_t value(const std::string& text)
    {
        auto [entry, inserted] =
            valueIndex.try_emplace(text, static_cast<std::uint32_t>(layer.values.size()));
        if (inserted)
        {
            layer.values.emplace_back(text);
        }
        return entry->second;
    }

    // A NUMBER, which the wire format distinguishes from its own decimal
    // spelling. Interned under a prefixed key so the integer 5 and the string
    // "5" cannot collapse into one entry -- a style comparing rank < 3 sees
    // nothing at all if its rank arrived as text.
    std::uint32_t number(std::int64_t n)
    {
        const std::string tag = "#" + std::to_string(n);
        auto [entry, inserted] =
            valueIndex.try_emplace(tag, static_cast<std::uint32_t>(layer.values.size()));
        if (inserted)
        {
            layer.values.emplace_back(n);
        }
        return entry->second;
    }
};

} // namespace

const char* layerFor(map_rules::RenderClass value)
{
    // The vocabulary dashboard/widgets/map/tessellator.cpp already switches on.
    // Getting a name wrong here does not fail: the layer is simply never drawn,
    // and the map comes up missing its water or its buildings with nothing said.
    switch (value)
    {
        case map_rules::RenderClass::None:
            return "";
        case map_rules::RenderClass::Motorway:
        case map_rules::RenderClass::Trunk:
        case map_rules::RenderClass::Primary:
        case map_rules::RenderClass::Secondary:
        case map_rules::RenderClass::Tertiary:
        case map_rules::RenderClass::Minor:
        case map_rules::RenderClass::Service:
        case map_rules::RenderClass::Track:
        case map_rules::RenderClass::Path:
        case map_rules::RenderClass::Pedestrian:
        case map_rules::RenderClass::Rail:
        case map_rules::RenderClass::Ferry:
            return "transportation";
        case map_rules::RenderClass::Waterway:
            return "waterway";
        case map_rules::RenderClass::Water:
            return "water";
        case map_rules::RenderClass::Building:
            return "building";
        case map_rules::RenderClass::Landuse:
            return "landuse";
        case map_rules::RenderClass::Landcover:
            return "landcover";
        case map_rules::RenderClass::Boundary:
            return "boundary";
        case map_rules::RenderClass::Place:
            // The label layer, which dashboard/widgets/map/labels.cpp reads by
            // this exact name.
            return "place";
    }
    return "";
}

const char* roadClassFor(map_rules::RenderClass value)
{
    // What roadPriority() in the tessellator reads. Anything it does not
    // recognise draws as a minor road, which is the right failure -- a hole in
    // the road network is worse than a road of the wrong width.
    switch (value)
    {
        case map_rules::RenderClass::Motorway:
            return "motorway";
        case map_rules::RenderClass::Trunk:
            return "trunk";
        case map_rules::RenderClass::Primary:
            return "primary";
        case map_rules::RenderClass::Secondary:
            return "secondary";
        case map_rules::RenderClass::Tertiary:
            return "tertiary";
        case map_rules::RenderClass::Rail:
            return "rail";
        case map_rules::RenderClass::Minor:
            return "minor";
        case map_rules::RenderClass::Service:
            return "service";
        case map_rules::RenderClass::Track:
            return "track";
        case map_rules::RenderClass::Path:
            return "path";
        case map_rules::RenderClass::Pedestrian:
            return "pedestrian";
        case map_rules::RenderClass::Ferry:
            return "ferry";
        case map_rules::RenderClass::None:
        case map_rules::RenderClass::Waterway:
        case map_rules::RenderClass::Water:
        case map_rules::RenderClass::Building:
        case map_rules::RenderClass::Landuse:
        case map_rules::RenderClass::Landcover:
        case map_rules::RenderClass::Boundary:
            // A place's class is its KIND (city, town, suburb), which comes
            // from PlaceClassification rather than from here -- the road
            // vocabulary has no word for it.
        case map_rules::RenderClass::Place:
            return "";
    }
    return "";
}

void Tiler::add(DrawInput&& feature)
{
    // Two values is a point; four is the shortest line. Anything less is a way
    // whose vertices did not resolve, and drawing it would put a road at Null
    // Island.
    const std::size_t minimum = feature.isPoint ? 2 : 4;
    if (feature.geometry.size() < minimum)
    {
        return;
    }

    Prepared prepared;
    prepared.isPoint = feature.isPoint;
    prepared.placeKind = feature.place.kind;
    prepared.labelRank = feature.place.labelRank;
    prepared.population = feature.place.population;
    prepared.renderClass = feature.classification.renderClass;
    prepared.className = feature.classification.className;
    prepared.adminLevel = feature.adminLevel;
    prepared.layer = feature.layer;
    prepared.attributes = std::move(feature.attributes);
    prepared.minZoom = feature.classification.minZoom;
    prepared.isArea = feature.classification.isArea;
    prepared.osmWayId = feature.osmWayId;
    prepared.name = std::move(feature.name);
    prepared.ref = std::move(feature.ref);
    prepared.postedKph = feature.classification.postedSpeedKph;
    prepared.hasPosted = feature.classification.hasPosted;
    prepared.isBridge = feature.classification.isBridge;
    prepared.isTunnel = feature.classification.isTunnel;
    prepared.osmLayer = feature.classification.layer;
    prepared.laneCount = feature.classification.laneCount;
    prepared.onewayForward = feature.classification.onewayForward;
    prepared.onewayBackward = feature.classification.onewayBackward;

    // Projected ONCE, here. Doing it per zoom would be the same arithmetic
    // fourteen times, and doing it twice anywhere is how a map ends up subtly
    // offset from itself.
    const auto project = [&](const std::vector<osm::Coord>& ring, bool inner) {
        if (ring.size() < 2)
        {
            return;
        }
        std::vector<double> out;
        out.reserve(ring.size());
        for (std::size_t i = 0; i + 1 < ring.size(); i += 2)
        {
            out.push_back(worldX(ring[i + 1]));
            out.push_back(worldY(ring[i]));
        }
        prepared.worldXY.push_back(std::move(out));
        prepared.ringIsInner.push_back(inner ? 1 : 0);
    };

    project(feature.geometry, false);
    for (const auto& ring : feature.outerRings)
    {
        project(ring, false);
    }
    for (const auto& ring : feature.innerRings)
    {
        project(ring, true);
    }
    if (prepared.worldXY.empty())
    {
        return;
    }

    mFeatures.push_back(std::move(prepared));
}

mbtiles::Result<TileStats> Tiler::write(mbtiles::Writer& writer, const TileOptions& options,
                                        const std::string& name, std::int32_t west,
                                        std::int32_t south, std::int32_t east, std::int32_t north)
{
    TileStats stats;
    stats.features = mFeatures.size();

    // What the `json` metadata column will say. Filled in as features survive
    // into tiles, never from the layer table up front -- see the note at the
    // point it is written.
    // What a layer turned out to contain, accumulated as it is written.
    //
    // The FIELD LIST is gathered rather than declared, because the answer is a
    // property of the data and not of the code: `ref` exists in transportation
    // only because some road in this extract had one, and advertising a field no
    // feature carries sends a style looking for something no tile will ever
    // have. It also means a new attribute reaches the metadata by being written,
    // with no second place to update -- which is the failure this replaced, a
    // hardcoded triple of class/name/ref that quietly omitted everything else.
    struct LayerSummary
    {
        std::uint8_t minZoom = 255;
        std::uint8_t maxZoom = 0;
        std::map<std::string, const char*> fields;

        void note(const std::string& key, const char* type) { fields[key] = type; }
    };
    std::map<std::string, LayerSummary> summaries;

    for (std::uint8_t z = options.minZoom; z <= options.maxZoom; ++z)
    {
        const double side = static_cast<double>(1U << z);
        const double extent = static_cast<double>(options.extent);

        // Half a tile unit, in world coordinates. Simplifying to finer than the
        // grid the tile is quantised to buys nothing at all.
        const double tolerance = 0.5 / (side * extent);

        std::unordered_map<TileKey, std::map<std::string, LayerBuilder>, TileKeyHash> tiles;

        std::vector<double> simplified;
        std::vector<std::vector<double>> simplifiedRings;
        std::vector<std::uint8_t> simplifiedInner;
        std::vector<std::vector<double>> parts;
        std::vector<std::uint8_t> partIsInner;

        for (const Prepared& feature : mFeatures)
        {
            if (z < feature.minZoom)
            {
                // The clutter dial, applied at build time. A style can only ever
                // be STRICTER than this: nothing can draw what the tile does not
                // carry.
                continue;
            }

            const char* layerName =
                feature.layer[0] == '\0' ? layerFor(feature.renderClass) : feature.layer;
            if (layerName[0] == '\0')
            {
                continue;
            }

            if (feature.isPoint)
            {
                // A LABEL. Neither simplified nor clipped, and emitted into
                // exactly one tile -- the one it falls in.
                //
                // Not duplicated into the neighbours the way a renderer's own
                // label buffer would: dashboard/widgets/map/labels.cpp gathers
                // candidates across every visible tile before placing any of
                // them, so a second copy would compete with the first for the
                // same spot and one of the two would always lose.
                const double px = feature.worldXY[0][0];
                const double py = feature.worldXY[0][1];
                const auto tx = static_cast<std::uint32_t>(
                    std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(px * side)), 0,
                                             static_cast<std::int64_t>(side) - 1));
                const auto ty = static_cast<std::uint32_t>(
                    std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(py * side)), 0,
                                             static_cast<std::int64_t>(side) - 1));

                auto& layers = tiles[TileKey { tx, ty }];
                LayerBuilder& builder = layers[layerName];
                if (builder.layer.name.empty())
                {
                    builder.layer.name = layerName;
                    builder.layer.version = 2;
                    builder.layer.extent = options.extent;
                }

                mvt::Feature out;
                out.type = mvt::GeomType::Point;
                out.hasId = true;
                out.id = static_cast<std::uint64_t>(feature.osmWayId);

                if (!feature.name.empty())
                {
                    out.tags.push_back(builder.key("name"));
                    out.tags.push_back(builder.value(feature.name));

                    // BOTH spellings, and the duplication is deliberate.
                    // dashboard/widgets/map/labels.cpp reads `name:latin`,
                    // because the archive tilemaker produced emitted only that
                    // and reading `name` returned an empty string for every
                    // place -- a map with no labels and no error anywhere.
                    // Writing both means the widget needs no change to read our
                    // tiles, and a future reader that asks for `name` is right
                    // too. It costs one varint pair per label.
                    out.tags.push_back(builder.key("name:latin"));
                    out.tags.push_back(builder.value(feature.name));
                }

                // A place says what it is with its kind; every other label
                // layer carries a className instead. Both land on `class`,
                // because that is the one attribute every style reads.
                const char* pointClass = feature.placeKind != map_rules::PlaceKind::None
                                             ? map_rules::to_string(feature.placeKind)
                                             : feature.className;
                if (pointClass[0] != '\0')
                {
                    out.tags.push_back(builder.key("class"));
                    out.tags.push_back(builder.value(pointClass));
                }
                if (feature.labelRank != 255)
                {
                    out.tags.push_back(builder.key("rank"));
                    out.tags.push_back(builder.number(feature.labelRank));
                }
                if (feature.population != 0)
                {
                    out.tags.push_back(builder.key("population"));
                    out.tags.push_back(builder.number(feature.population));
                }
                for (const auto& [key, value] : feature.attributes)
                {
                    out.tags.push_back(builder.key(key));
                    out.tags.push_back(builder.value(value));
                }

                out.rings.push_back({ mvt::Point {
                    static_cast<std::int32_t>(std::llround((px * side - tx) * extent)),
                    static_cast<std::int32_t>(std::llround((py * side - ty) * extent)) } });

                builder.layer.features.push_back(std::move(out));

                LayerSummary& summary = summaries[layerName];
                summary.minZoom = std::min(summary.minZoom, z);
                summary.maxZoom = std::max(summary.maxZoom, z);
                if (pointClass[0] != '\0')
                {
                    summary.note("class", "String");
                }
                if (!feature.name.empty())
                {
                    summary.note("name", "String");
                    summary.note("name:latin", "String");
                }
                if (feature.labelRank != 255)
                {
                    summary.note("rank", "Number");
                }
                if (feature.population != 0)
                {
                    summary.note("population", "Number");
                }
                for (const auto& [key, value] : feature.attributes)
                {
                    (void)value;
                    summary.note(key, "String");
                }
                continue;
            }

            // Every ring simplified, and every ring kept or dropped on its own
            // merits. A hole too small to draw is dropped while its outer ring
            // survives, which is right: the alternative is a lake with a
            // one-pixel island stamped out of it.
            simplifiedRings.clear();
            simplifiedInner.clear();
            for (std::size_t r = 0; r < feature.worldXY.size(); ++r)
            {
                simplify(feature.worldXY[r], tolerance, simplified);
                // Four values is two points, the shortest line. A polygon needs
                // three points, or it is a ring with no inside.
                if (simplified.size() < (feature.isArea ? 6u : 4u))
                {
                    continue;
                }
                simplifiedRings.push_back(simplified);
                simplifiedInner.push_back(feature.ringIsInner[r]);
            }
            // An area whose FIRST surviving ring is a hole has lost its outside;
            // there is nothing left to fill.
            if (simplifiedRings.empty() || (feature.isArea && simplifiedInner[0] != 0))
            {
                ++stats.droppedTooSmall;
                continue;
            }

            // Bounding box -> tile range. The cheap answer, and the right one:
            // walking the line to find only the tiles it really crosses saves
            // work on long diagonals and costs more everywhere else.
            double minX = simplifiedRings[0][0];
            double maxX = simplifiedRings[0][0];
            double minY = simplifiedRings[0][1];
            double maxY = simplifiedRings[0][1];
            for (const std::vector<double>& ring : simplifiedRings)
            {
                for (std::size_t i = 0; i + 1 < ring.size(); i += 2)
                {
                    minX = std::min(minX, ring[i]);
                    maxX = std::max(maxX, ring[i]);
                    minY = std::min(minY, ring[i + 1]);
                    maxY = std::max(maxY, ring[i + 1]);
                }
            }

            // GENERALIZATION: drop what is smaller than the grid it would be
            // drawn on.
            //
            // Simplification thins a shape's points but never removes the shape
            // itself, so without this every cul-de-sac and back garden in the
            // region is carried at every zoom -- at z9 that was an eight-fold
            // size increase over the tiles this replaced, for detail occupying
            // well under one pixel.
            //
            // The threshold is expressed in TILE UNITS so it means the same
            // thing at every zoom: a feature whose whole bounding box is
            // narrower than `minExtent` units cannot be told from a dot.
            // Applied to the bounding box rather than to area, so that a long
            // thin road survives while a small compact blob does not.
            const double boxUnits =
                std::max(maxX - minX, maxY - minY) * side * extent;
            if (boxUnits < options.minExtent)
            {
                ++stats.droppedTooSmall;
                continue;
            }

            const auto clampTile = [&](double v) {
                const auto t = static_cast<std::int64_t>(std::floor(v * side));
                return static_cast<std::uint32_t>(
                    std::clamp<std::int64_t>(t, 0, static_cast<std::int64_t>(side) - 1));
            };

            const std::uint32_t x0 = clampTile(minX);
            const std::uint32_t x1 = clampTile(maxX);
            const std::uint32_t y0 = clampTile(minY);
            const std::uint32_t y1 = clampTile(maxY);

            // A feature spanning a huge tile range at high zoom is a coastline
            // or a boundary; emitting it into ten thousand tiles would dominate
            // the build for something nobody can see moving.
            if ((static_cast<std::uint64_t>(x1 - x0) + 1) * (y1 - y0 + 1) > 4096)
            {
                ++stats.droppedTooSmall;
                continue;
            }

            for (std::uint32_t tx = x0; tx <= x1; ++tx)
            {
                for (std::uint32_t ty = y0; ty <= y1; ++ty)
                {
                    // The tile's own rectangle, in world coordinates, grown by
                    // the buffer. Without the buffer every line stops dead at
                    // the boundary and the renderer's joins leave a seam.
                    const double bufferWorld = options.buffer / (side * extent);
                    const Rect rect { tx / side - bufferWorld, ty / side - bufferWorld,
                                      (tx + 1) / side + bufferWorld,
                                      (ty + 1) / side + bufferWorld };

                    parts.clear();
                    partIsInner.clear();
                    if (feature.isArea)
                    {
                        for (std::size_t r = 0; r < simplifiedRings.size(); ++r)
                        {
                            auto clipped = clipPolygon(simplifiedRings[r], rect);
                            if (clipped.size() >= 6)
                            {
                                parts.push_back(std::move(clipped));
                                partIsInner.push_back(simplifiedInner[r]);
                            }
                        }
                        // A hole that survived while its outer ring was clipped
                        // away would be filled as if it were the shape.
                        if (!parts.empty() && partIsInner[0] != 0)
                        {
                            parts.clear();
                        }
                    }
                    else
                    {
                        for (const std::vector<double>& ring : simplifiedRings)
                        {
                            clipLine(ring, rect, parts);
                        }
                        partIsInner.assign(parts.size(), 0);
                    }

                    if (parts.empty())
                    {
                        continue;
                    }

                    auto& layers = tiles[TileKey { tx, ty }];
                    LayerBuilder& builder = layers[layerName];
                    if (builder.layer.name.empty())
                    {
                        builder.layer.name = layerName;
                        builder.layer.version = 2;
                        builder.layer.extent = options.extent;
                    }

                    mvt::Feature out;
                    out.type = feature.isArea ? mvt::GeomType::Polygon : mvt::GeomType::LineString;

                    // THE SOURCE OSM WAY ID, stamped into the tile.
                    //
                    // This is what lets a client recolour the road it is on, or
                    // draw a route by highlighting the features it already has,
                    // instead of overlaying a full-precision polyline that
                    // visibly diverges from the simplified geometry at low zoom.
                    // It costs a varint per feature.
                    //
                    // A WAY ID AND NOT A SEGMENT ID, deliberately, and the two
                    // are not interchangeable. The tiler draws the whole way
                    // unsplit, while the graph splits it at every junction, so
                    // one drawn feature covers many segments and no single
                    // segment id describes it. The graph's WayIndex section is
                    // the join in that direction, and SegmentId carries the way
                    // id in its high bits for the other -- so nothing is lost
                    // by stamping the coarser of the two.
                    if (feature.osmWayId != 0)
                    {
                        out.hasId = true;
                        out.id = static_cast<std::uint64_t>(feature.osmWayId);
                    }

                    // A CLASS NAME WINS OVER THE RENDER CLASS, and the order
                    // matters: a runway is carried with a road's render class so
                    // it simplifies and clips like a line, but its class is
                    // "runway" and not "service". Roads set no class name and so
                    // fall through to the enum, where the render class IS the
                    // road class.
                    const char* roadClass = feature.className;
                    if (roadClass[0] == '\0')
                    {
                        roadClass = roadClassFor(feature.renderClass);
                    }
                    if (roadClass[0] != '\0')
                    {
                        out.tags.push_back(builder.key("class"));
                        out.tags.push_back(builder.value(roadClass));
                    }
                    if (!feature.name.empty())
                    {
                        out.tags.push_back(builder.key("name"));
                        out.tags.push_back(builder.value(feature.name));
                        // Both spellings, for the same reason the label layer
                        // writes both -- see the point path above.
                        out.tags.push_back(builder.key("name:latin"));
                        out.tags.push_back(builder.value(feature.name));
                    }
                    if (!feature.ref.empty())
                    {
                        out.tags.push_back(builder.key("ref"));
                        out.tags.push_back(builder.value(feature.ref));
                    }
                    if (feature.adminLevel != 0)
                    {
                        out.tags.push_back(builder.key("admin_level"));
                        out.tags.push_back(builder.number(feature.adminLevel));
                    }
                    // The posted limit, where OSM records one.
                    //
                    // ONLY AT AND ABOVE THE MERGE THRESHOLD, and that is the
                    // whole subtlety. Below it, mergeLines() folds line features
                    // that share byte-identical tags into one multi-part
                    // feature, and that is what keeps low-zoom tiles small.
                    // A per-road speed splits roads that would otherwise merge,
                    // so writing it everywhere would inflate exactly the tiles
                    // generalization exists to shrink -- to buy an attribute
                    // that means nothing at continental zoom anyway.
                    //
                    // `hasPosted` and not `postedKph != 0`: absence of the key
                    // has to mean "not tagged", which is a different fact from
                    // a limit of zero and is the one a consumer must not guess.
                    if (feature.hasPosted && z >= options.mergeBelowZoom)
                    {
                        out.tags.push_back(builder.key("maxspeed"));
                        out.tags.push_back(builder.number(feature.postedKph));
                    }
                    // Grade separation, and the rest of the per-road detail.
                    //
                    // All of it behind the same merge threshold as `maxspeed`,
                    // and for the same reason: mergeLines() folds features with
                    // byte-identical tags into one, so a per-road attribute
                    // splits roads that would otherwise merge and inflates the
                    // low-zoom tiles generalization exists to shrink. None of
                    // this means anything at continental zoom anyway -- an
                    // overpass is a few pixels of a grey smear.
                    if (z >= options.mergeBelowZoom)
                    {
                        // `brunnel`, spelled the way OpenMapTiles spells it,
                        // because the whole schema is built to that vocabulary
                        // and the widget's style names follow it. Absent means
                        // at grade, which is the overwhelming majority -- a key
                        // written on every road would cost more than it says.
                        if (feature.isBridge)
                        {
                            out.tags.push_back(builder.key("brunnel"));
                            out.tags.push_back(builder.value("bridge"));
                        }
                        else if (feature.isTunnel)
                        {
                            out.tags.push_back(builder.key("brunnel"));
                            out.tags.push_back(builder.value("tunnel"));
                        }
                        // OSM's own `layer`, which is what stacks one bridge
                        // over another. Zero is the default and is not written.
                        if (feature.osmLayer != 0)
                        {
                            out.tags.push_back(builder.key("layer"));
                            out.tags.push_back(builder.number(feature.osmLayer));
                        }
                        if (feature.laneCount != 0)
                        {
                            out.tags.push_back(builder.key("lanes"));
                            out.tags.push_back(builder.number(feature.laneCount));
                        }
                        // 1 forward, -1 backward, absent for two-way. The same
                        // encoding OpenMapTiles uses, and the reason -1 exists
                        // rather than a second key is that a way's direction is
                        // its geometry's direction.
                        if (feature.onewayForward != feature.onewayBackward)
                        {
                            out.tags.push_back(builder.key("oneway"));
                            out.tags.push_back(builder.number(feature.onewayForward ? 1 : -1));
                        }
                    }
                    for (const auto& [key, value] : feature.attributes)
                    {
                        out.tags.push_back(builder.key(key));
                        out.tags.push_back(builder.value(value));
                    }

                    for (std::size_t p = 0; p < parts.size(); ++p)
                    {
                        const std::vector<double>& part = parts[p];
                        std::vector<mvt::Point> ring;
                        ring.reserve(part.size() / 2);
                        for (std::size_t i = 0; i + 1 < part.size(); i += 2)
                        {
                            // World -> tile-local. Rounded rather than
                            // truncated: truncation biases every coordinate
                            // half a unit towards the tile origin, which over a
                            // whole pyramid reads as a systematic offset.
                            ring.push_back(mvt::Point {
                                static_cast<std::int32_t>(
                                    std::llround((part[i] * side - tx) * extent)),
                                static_cast<std::int32_t>(
                                    std::llround((part[i + 1] * side - ty) * extent)) });
                        }
                        // A LINE needs two points; a RING needs three. The
                        // encoder enforces this too, and does so as the last
                        // line of defence for the whole tile -- see the note in
                        // mvt/encode.cpp. Here it merely avoids building
                        // geometry that would be thrown away.
                        const std::size_t needed = feature.isArea ? 3 : 2;
                        if (ring.size() < needed)
                        {
                            continue;
                        }

                        // WINDING IS SET HERE, AND ONLY HERE.
                        //
                        // The vector tile format carries no flag for a hole:
                        // the only thing separating an island from the lake
                        // around it is which way its ring turns. An exterior
                        // ring must have positive area in tile coordinates
                        // (mvt::isExteriorRing), an interior ring negative.
                        //
                        // The roles come from the relation, and they have been
                        // carried this far as roles precisely so this decision
                        // happens after projection -- Web Mercator's y grows
                        // southward, so a ring's sign flips somewhere between
                        // latitude and the tile, and deciding earlier means
                        // deciding twice.
                        //
                        // Get it backwards and the renderer fills the island and
                        // punches out the lake.
                        if (feature.isArea)
                        {
                            const bool wantExterior = partIsInner[p] == 0;
                            if (mvt::isExteriorRing(ring) != wantExterior)
                            {
                                std::reverse(ring.begin(), ring.end());
                            }
                        }

                        out.rings.push_back(std::move(ring));
                    }

                    if (!out.rings.empty())
                    {
                        // Which layers exist, and over which zooms, is not
                        // knowable until the pyramid is built -- a layer whose
                        // every feature was clipped away or simplified to
                        // nothing must not be advertised. So it is recorded
                        // here, where a feature actually survived into a tile,
                        // and written out as `json` at the end.
                        LayerSummary& summary = summaries[layerName];
                        summary.minZoom = std::min(summary.minZoom, z);
                        summary.maxZoom = std::max(summary.maxZoom, z);
                        if (roadClass[0] != '\0')
                        {
                            summary.note("class", "String");
                        }
                        if (!feature.name.empty())
                        {
                            summary.note("name", "String");
                            summary.note("name:latin", "String");
                        }
                        if (!feature.ref.empty())
                        {
                            summary.note("ref", "String");
                        }
                        if (feature.adminLevel != 0)
                        {
                            summary.note("admin_level", "Number");
                        }
                        for (const auto& [key, value] : feature.attributes)
                        {
                            (void)value;
                            summary.note(key, "String");
                        }

                        builder.layer.features.push_back(std::move(out));
                    }
                }
            }
        }

        for (auto& [key, layers] : tiles)
        {
            mvt::Tile tile;
            for (auto& [layerName, builder] : layers)
            {
                if (!builder.layer.features.empty())
                {
                    mergeLines(builder.layer, z, options.mergeBelowZoom, stats);
                    tile.layers.push_back(std::move(builder.layer));
                }
            }
            if (tile.layers.empty())
            {
                ++stats.emptyTiles;
                continue;
            }

            auto encoded = mvt::encode(tile);
            if (!encoded)
            {
                return mbtiles::query_error("tile " + std::to_string(z) + "/" +
                                                std::to_string(key.x) + "/" +
                                                std::to_string(key.y) + ": " +
                                                mvt::to_string(encoded.error()),
                                            0);
            }

            auto compressed = mvt::gzipCompress(*encoded);
            if (!compressed)
            {
                return mbtiles::query_error("gzip failed", 0);
            }

            if (auto ok = writer.put(z, key.x, key.y, *compressed); !ok)
            {
                return std::unexpected(ok.error());
            }

            ++stats.tiles;
            ++stats.tilesPerZoom[z];
            stats.bytes += compressed->size();

            if (options.progressEvery != 0 && stats.tiles % options.progressEvery == 0)
            {
                SPDLOG_INFO("[tile] z{} -- {} tiles, {} MB", z, stats.tiles,
                            stats.bytes / (1024 * 1024));
            }
        }

        SPDLOG_INFO("[tile] z{} done: {} tiles", z, stats.tilesPerZoom[z]);
    }

    // Metadata. `format`, `minzoom` and `maxzoom` are what every client reads
    // first; bounds and center are what a picker uses to know where to look.
    const auto degrees = [](std::int32_t value) {
        return std::to_string(static_cast<double>(value) * kCoordScale);
    };

    if (auto ok = writer.setMetadata("name", name); !ok)
    {
        return std::unexpected(ok.error());
    }
    if (auto ok = writer.setMetadata("format", "pbf"); !ok)
    {
        return std::unexpected(ok.error());
    }
    if (auto ok = writer.setMetadata("type", "baselayer"); !ok)
    {
        return std::unexpected(ok.error());
    }
    if (auto ok = writer.setMetadata("version", "3.0"); !ok)
    {
        return std::unexpected(ok.error());
    }
    if (auto ok = writer.setMetadata("minzoom", std::to_string(options.minZoom)); !ok)
    {
        return std::unexpected(ok.error());
    }
    if (auto ok = writer.setMetadata("maxzoom", std::to_string(options.maxZoom)); !ok)
    {
        return std::unexpected(ok.error());
    }
    if (auto ok = writer.setMetadata("bounds", degrees(west) + "," + degrees(south) + "," +
                                                   degrees(east) + "," + degrees(north));
        !ok)
    {
        return std::unexpected(ok.error());
    }
    if (auto ok = writer.setMetadata(
            "center", degrees(static_cast<std::int32_t>(
                          (static_cast<std::int64_t>(west) + east) / 2)) +
                          "," +
                          degrees(static_cast<std::int32_t>(
                              (static_cast<std::int64_t>(south) + north) / 2)) +
                          ",10");
        !ok)
    {
        // Widened before adding: two longitudes near -118 degrees sum to about
        // -2.4e9, which overflows int32 and would put the centre in the wrong
        // hemisphere. The archive tilemaker produced has exactly this bug in
        // its own metadata, which is why the widget defaults to Irvine rather
        // than reading it.
        return std::unexpected(ok.error());
    }

    // `vector_layers`, which Archive::tileJson() lifts to the top level of the
    // TileJSON it hands a client. Without it a style has no way to know what is
    // in the archive, and every layer-driven client renders an empty map with
    // nothing in the logs.
    //
    // Built from what was actually written, not from the layer table: a layer
    // whose every feature was clipped away or simplified out of existence must
    // not be advertised, or a client waits for something no tile will ever
    // carry.
    nlohmann::json layerList = nlohmann::json::array();
    for (const auto& [layerName, summary] : summaries)
    {
        nlohmann::json fields = nlohmann::json::object();
        for (const auto& [key, type] : summary.fields)
        {
            fields[key] = type;
        }

        layerList.push_back(nlohmann::json {
            { "id", layerName },
            { "minzoom", summary.minZoom },
            { "maxzoom", summary.maxZoom },
            { "fields", std::move(fields) },
        });
    }

    if (auto ok = writer.setMetadata("json",
                                     nlohmann::json { { "vector_layers", std::move(layerList) } }
                                         .dump());
        !ok)
    {
        return std::unexpected(ok.error());
    }

    return stats;
}

} // namespace map_build
