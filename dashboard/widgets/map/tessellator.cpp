// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/tessellator.h"

// earcut.hpp shadows its own members in a dozen places, which our -Werror
// rejects. Silenced for the header alone rather than for the file: -Wshadow
// still applies to everything below, and rather than by marking the include
// SYSTEM, which is the option third_party/earcut.cmake explains away.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <mapbox/earcut.hpp>
#pragma GCC diagnostic pop

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <span>
#include <variant>

// Teach earcut how to read an mvt::Point, so rings go in without a copy.
namespace mapbox::util
{
template <>
struct nth<0, mvt::Point>
{
    static std::int32_t get(const mvt::Point& p) { return p.x; }
};
template <>
struct nth<1, mvt::Point>
{
    static std::int32_t get(const mvt::Point& p) { return p.y; }
};
} // namespace mapbox::util

namespace map_widget
{
namespace
{

struct Colour
{
    float r, g, b, a;
};

Colour toRgba(const helpers::Color& colour)
{
    // "#rgb", "#rrggbb" or "#rrggbbaa"; anything else paints magenta rather
    // than transparent, because an invisible layer reads as missing data and a
    // magenta one reads as a bad colour string.
    const std::string& text = colour.value();
    const auto hex = [&](std::size_t i, std::size_t n) -> float {
        unsigned value = 0;
        for (std::size_t k = 0; k < n; ++k)
        {
            const char c = text[i + k];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') value |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= unsigned(c - 'A' + 10);
        }
        if (n == 1) value = (value << 4) | value;
        return float(value) / 255.0f;
    };

    if (!helpers::Color::isValidFormat(text))
    {
        return { 1.f, 0.f, 1.f, 1.f };
    }

    const std::size_t digits = text.size() - 1;
    if (digits == 3)
    {
        return { hex(1, 1), hex(2, 1), hex(3, 1), 1.f };
    }
    if (digits == 8)
    {
        return { hex(1, 2), hex(3, 2), hex(5, 2), hex(7, 2) };
    }
    return { hex(1, 2), hex(3, 2), hex(5, 2), 1.f };
}

bool isRunway(const mvt::Layer& layer, const mvt::Feature& feature)
{
    return layer.attributeTextView(feature, "class") == "runway";
}
bool isNotRunway(const mvt::Layer& layer, const mvt::Feature& feature)
{
    return !isRunway(layer, feature);
}

// `brunnel` is OpenMapTiles' word for grade separation, and absent means at
// grade -- which is the overwhelming majority of roads, so the key is only
// written where it says something.
bool isBridge(const mvt::Layer& layer, const mvt::Feature& feature)
{
    return layer.attributeTextView(feature, "brunnel") == "bridge";
}
bool isNotBridge(const mvt::Layer& layer, const mvt::Feature& feature)
{
    return !isBridge(layer, feature);
}

// Defined below, next to the layer colour and width they borrow from.
Colour bridgeColour(const mvt::Layer& layer, const mvt::Feature& feature, const MapStyle_t& style);
Colour bridgeCasingColour(const mvt::Layer& layer, const mvt::Feature& feature,
                          const MapStyle_t& style);
float bridgeHalfWidth(const mvt::Layer& layer, const mvt::Feature& feature,
                      const MapStyle_t& style);
float bridgeCasingHalfWidth(const mvt::Layer& layer, const mvt::Feature& feature,
                            const MapStyle_t& style);
float boundaryHalfWidth(const mvt::Layer& layer, const mvt::Feature& feature,
                        const MapStyle_t& style);

struct LayerSpec
{
    MapLayer layer;
    const char* sourceLayer;
    int roadClass;      // 0 when the source layer is the whole filter
    bool fill;
    // Per-feature filter for source layers that are neither `transportation`
    // nor taken whole. Null takes everything of the right geometry type.
    //
    // Takes the whole feature rather than just its class, because the useful
    // predicate is sometimes a NEGATION ("every aeroway line that is not a
    // runway") and sometimes reads a different key entirely ("every road that
    // is not on a bridge").
    bool (*featureFilter)(const mvt::Layer&, const mvt::Feature&) { nullptr };
    // Half-width and colour per FEATURE, overriding the layer's own. Null means
    // every feature in the layer looks the same, which is true of most.
    //
    // Both ride the VERTEX rather than a uniform -- `halfPx` and the colour are
    // already per-vertex, which is exactly what they are for -- so a layer
    // whose features differ needs no extra draw call and no extra layer. That
    // is what lets the two bridge layers carry roads of every class.
    float (*featureHalfWidth)(const mvt::Layer&, const mvt::Feature&, const MapStyle_t&) { nullptr };
    Colour (*featureColour)(const mvt::Layer&, const mvt::Feature&, const MapStyle_t&) { nullptr };
};

constexpr std::array<LayerSpec, kMapLayerCount> kSpecs { {
    { MapLayer::Landcover,      "landcover",      0, true  },
    { MapLayer::Landuse,        "landuse",        0, true  },
    { MapLayer::Park,           "park",           0, true  },
    { MapLayer::AerowaySurface, "aeroway",        0, true  },
    { MapLayer::Water,          "water",          0, true  },
    { MapLayer::Waterway,       "waterway",       0, false },
    { MapLayer::Building,       "building",       0, true  },
    { MapLayer::AerowayTaxiway, "aeroway",        0, false, isNotRunway },
    { MapLayer::AerowayRunway,  "aeroway",        0, false, isRunway    },
    // The road network AT GRADE. Bridges are excluded and drawn again above, so
    // a road is in exactly one of the two sets -- drawing it in both would put
    // a copy of every bridge under the traffic it crosses, which is the artefact
    // the bridge layers exist to remove.
    { MapLayer::RoadMinor,      "transportation", 1, false, isNotBridge },
    { MapLayer::RoadMajor,      "transportation", 2, false, isNotBridge },
    { MapLayer::RoadPrimary,    "transportation", 3, false, isNotBridge },
    { MapLayer::MotorwayCasing, "transportation", 4, false, isNotBridge },
    { MapLayer::Motorway,       "transportation", 4, false, isNotBridge },
    { MapLayer::Rail,           "transportation", 5, false, isNotBridge },
    { MapLayer::RoadBridgeCasing, "transportation", 0, false, isBridge, bridgeCasingHalfWidth,
      bridgeCasingColour },
    { MapLayer::RoadBridge,     "transportation", 0, false, isBridge, bridgeHalfWidth,
      bridgeColour },
    { MapLayer::Boundary,       "boundary",       0, false, nullptr, boundaryHalfWidth },
    // From the tracks archive rather than the basemap. The source layer names
    // are disjoint from the OpenMapTiles sixteen by construction, which is what
    // lets two archives be drawn through one tessellator without either
    // shadowing the other.
    { MapLayer::TrackSurface,   "track",            0, true  },
    { MapLayer::TrackCentre,    "track_centerline", 0, false },
} };

// kSpecs is indexed by the ENUM ORDINAL in tessellate(), and nothing else
// checks that the rows are in enum order. A row out of place silently
// tessellates one layer's features into another layer's slot.
static_assert([] {
    for (std::size_t i = 0; i < kSpecs.size(); ++i)
    {
        if (kSpecs[i].layer != static_cast<MapLayer>(i))
        {
            return false;
        }
    }
    return true;
}(), "kSpecs rows must be in MapLayer order");

Colour colourFor(MapLayer layer, const MapStyle_t& style)
{
    switch (layer)
    {
        case MapLayer::Landcover:      return toRgba(style.landcover);
        case MapLayer::Landuse:        return toRgba(style.landuse);
        case MapLayer::Park:           return toRgba(style.park);
        case MapLayer::AerowaySurface: return toRgba(style.aeroway_surface);
        case MapLayer::Water:          return toRgba(style.water);
        case MapLayer::Waterway:       return toRgba(style.waterway);
        case MapLayer::Building:       return toRgba(style.building);
        case MapLayer::AerowayTaxiway: return toRgba(style.aeroway_line);
        case MapLayer::AerowayRunway:  return toRgba(style.aeroway_line);
        case MapLayer::RoadMinor:      return toRgba(style.road_minor);
        case MapLayer::RoadMajor:      return toRgba(style.road_major);
        case MapLayer::RoadPrimary:    return toRgba(style.road_primary);
        case MapLayer::MotorwayCasing: return toRgba(style.motorway_casing);
        case MapLayer::Motorway:       return toRgba(style.motorway);
        case MapLayer::Rail:           return toRgba(style.rail);
        // Only a fallback: both bridge layers override per feature, because
        // they carry roads of every class.
        case MapLayer::RoadBridgeCasing: return toRgba(style.bridge_casing);
        case MapLayer::RoadBridge:     return toRgba(style.road_minor);
        case MapLayer::Boundary:       return toRgba(style.boundary);
        case MapLayer::TrackSurface:   return toRgba(style.racetrack_surface);
        case MapLayer::TrackCentre:    return toRgba(style.racetrack_centre);
    }
    return { 1.f, 0.f, 1.f, 1.f };
}

// A national border is a heavier line than a city limit. map_build writes OSM's
// own `admin_level` -- 2/4/6/8 for country/state/county/city, with odd levels
// between them taking the coarser neighbour (tools/map_build/extract.cpp) -- and
// without reading it every border on the map is one weight.
//
// Fractions of `widths.boundary` rather than four style fields, so that the one
// knob still sets the weight of the whole set and setting it to zero still hides
// all of them. The ladder is deliberately shallow: a county line thinner than
// about a third of a country line stops being visible at all against landcover.
float boundaryHalfWidth(const mvt::Layer& layer, const mvt::Feature& feature,
                        const MapStyle_t& style)
{
    const float full = float(style.widths.boundary);
    const std::optional<double> level = attributeNumber(layer, feature, "admin_level");
    if (!level.has_value())
    {
        // An archive that does not write the attribute keeps the single weight
        // it had before, rather than dropping to the thinnest.
        return full;
    }

    if (*level <= 2.0) return full;            // country
    if (*level <= 4.0) return full * 0.70f;    // state or province
    if (*level <= 6.0) return full * 0.50f;    // county
    return full * 0.38f;                       // city and below
}

// A bridge is the same road, drawn higher up: its colour and width are its own
// class's, borrowed rather than restated so the two cannot drift apart.
Colour bridgeColour(const mvt::Layer& layer, const mvt::Feature& feature, const MapStyle_t& style)
{
    return colourFor(roadLayerFor(roadPriority(layer.attributeTextView(feature, "class"))), style);
}

float bridgeHalfWidth(const mvt::Layer& layer, const mvt::Feature& feature, const MapStyle_t& style)
{
    return halfWidthFor(roadLayerFor(roadPriority(layer.attributeTextView(feature, "class"))), style);
}

// The casing is what separates the deck from whatever it crosses. One colour
// for every class -- a bridge edge is a shadow, not a road -- and a fixed extra
// width either side, so a wide road gets the same visual lip as a narrow one.
Colour bridgeCasingColour(const mvt::Layer& layer, const mvt::Feature& feature,
                          const MapStyle_t& style)
{
    (void)layer;
    (void)feature;
    return toRgba(style.bridge_casing);
}

float bridgeCasingHalfWidth(const mvt::Layer& layer, const mvt::Feature& feature,
                            const MapStyle_t& style)
{
    const float fill = bridgeHalfWidth(layer, feature, style);
    // A zero-width road stays hidden on its bridge too, rather than showing as
    // a casing with nothing in it.
    return fill > 0.0f ? fill + float(style.widths.bridge_casing) : 0.0f;
}

bool layerEnabled(MapLayer layer, const MapStyle_t& style)
{
    switch (layer)
    {
        case MapLayer::Building: return style.show_buildings;
        case MapLayer::Boundary: return style.show_boundaries;
        case MapLayer::RoadBridgeCasing:
        case MapLayer::RoadBridge:
            return style.show_bridges;
        case MapLayer::AerowaySurface:
        case MapLayer::AerowayTaxiway:
        case MapLayer::AerowayRunway:
            return style.show_aeroways;
        case MapLayer::TrackSurface:
        case MapLayer::TrackCentre:
            return style.show_racetracks;
        case MapLayer::Landcover:
        case MapLayer::Landuse:
        case MapLayer::Park:
        case MapLayer::Water:
        case MapLayer::Waterway:
        case MapLayer::RoadMinor:
        case MapLayer::RoadMajor:
        case MapLayer::RoadPrimary:
        case MapLayer::MotorwayCasing:
        case MapLayer::Motorway:
        case MapLayer::Rail:
            return true;
    }
    return true;
}

bool accepts(const LayerSpec& spec, const mvt::Layer& layer, const mvt::Feature& feature)
{
    if (spec.featureFilter != nullptr && !spec.featureFilter(layer, feature))
    {
        return false;
    }
    if (spec.roadClass == 0)
    {
        return true;
    }
    return roadPriority(layer.attributeTextView(feature, "class")) == spec.roadClass;
}

// ---------------------------------------------------------------- polylines

// A polyline, expanded to a triangle strip with mitre joins.
//
// Each vertex gets ONE normal shared by the segments either side of it, so the
// quads meet exactly instead of leaving a notch. The mitre length grows without
// bound as the turn approaches a hairpin -- 1/sin(theta/2) -- so past a limit it
// falls back to the segment normal, which bevels the corner. Unbounded mitres
// on a switchback produce spikes several hundred pixels long.
constexpr float kMitreLimit = 4.0f;

void emitPolyline(std::vector<MapVertex>& out, std::vector<std::uint32_t>& indices,
                  const std::vector<mvt::Point>& ring, float sc, float halfPx, const Colour& c)
{
    if (ring.size() < 2)
    {
        return;
    }

    const std::size_t n = ring.size();
    std::vector<std::pair<float, float>> pts;
    pts.reserve(n);
    for (const mvt::Point& p : ring)
    {
        const float x = float(p.x) * sc;
        const float y = float(p.y) * sc;
        // Drop repeated points: a zero-length segment has no direction, and its
        // normal comes out NaN.
        if (pts.empty() || std::abs(pts.back().first - x) > 0.f ||
            std::abs(pts.back().second - y) > 0.f)
        {
            pts.emplace_back(x, y);
        }
    }
    if (pts.size() < 2)
    {
        return;
    }

    const std::size_t m = pts.size();
    std::vector<std::pair<float, float>> segNormal(m - 1);
    for (std::size_t i = 0; i + 1 < m; ++i)
    {
        float dx = pts[i + 1].first - pts[i].first;
        float dy = pts[i + 1].second - pts[i].second;
        const float len = std::sqrt((dx * dx) + (dy * dy));
        dx /= len;
        dy /= len;
        segNormal[i] = { -dy, dx };
    }

    // One normal per point, mitred where two segments meet.
    std::vector<std::pair<float, float>> normal(m);
    normal.front() = segNormal.front();
    normal.back() = segNormal.back();
    for (std::size_t i = 1; i + 1 < m; ++i)
    {
        const auto& a = segNormal[i - 1];
        const auto& b = segNormal[i];
        float mx = a.first + b.first;
        float my = a.second + b.second;
        const float len = std::sqrt((mx * mx) + (my * my));
        if (len < 1e-6f)
        {
            // Doubling back on itself; there is no mitre.
            normal[i] = b;
            continue;
        }
        mx /= len;
        my /= len;
        const float cosHalf = (mx * b.first) + (my * b.second);
        const float scale = (std::abs(cosHalf) < 1e-6f) ? kMitreLimit : (1.0f / cosHalf);
        if (scale > kMitreLimit)
        {
            normal[i] = b;   // bevel
        }
        else
        {
            normal[i] = { mx * scale, my * scale };
        }
    }

    // TWO vertices per point -- one either side of the centreline -- and the
    // segments between them drawn by index.
    //
    // The normals are already per POINT rather than per segment (that is what
    // makes the joins mitre), so the vertex a segment would have written is
    // byte-for-byte the one its neighbour already wrote. Sharing them is
    // therefore not an approximation: the triangles that come out are the same
    // triangles, from a third of the memory.
    const auto base = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < m; ++i)
    {
        const auto& point = pts[i];
        const auto& side = normal[i];
        out.push_back(MapVertex { point.first, point.second, side.first, side.second, halfPx, c.r,
                                  c.g, c.b, c.a });
        out.push_back(MapVertex { point.first, point.second, -side.first, -side.second, halfPx, c.r,
                                  c.g, c.b, c.a });
    }

    for (std::size_t i = 0; i + 1 < m; ++i)
    {
        const auto quad = base + static_cast<std::uint32_t>(2 * i);
        // Same winding the expanded form used: (+n0, -n0, +n1) then
        // (+n1, -n0, -n1). Nothing culls, so this is about matching the old
        // output exactly rather than about facing.
        indices.push_back(quad + 0);
        indices.push_back(quad + 1);
        indices.push_back(quad + 2);
        indices.push_back(quad + 2);
        indices.push_back(quad + 1);
        indices.push_back(quad + 3);
    }
}

// ----------------------------------------------------------------- polygons

// MVT hands rings out flat: an exterior ring, then its holes, then the next
// exterior ring. Winding is the ONLY thing that says which is which, so the
// rings have to be regrouped into polygons before they can be triangulated.
void emitPolygon(std::vector<MapVertex>& out, std::vector<std::uint32_t>& indices,
                 const std::vector<std::span<const mvt::Point>>& polygon, float sc, const Colour& c)
{
    if (polygon.empty() || polygon.front().size() < 3)
    {
        return;
    }

    // REUSED, one per thread. mapbox::earcut<N>(poly) is a convenience wrapper
    // that constructs a fresh Earcut per call and moves its indices out; the
    // object owns a node pool and an index vector that then have to be built
    // again for the next polygon, and a city tile is thousands of polygons.
    // operator() clears its own state on entry and only allocates the pool when
    // it is empty, so reuse is what the class is written for. Tessellation runs
    // on the tile worker pool, hence thread_local rather than static.
    thread_local mapbox::detail::Earcut<std::uint32_t> earcut;
    earcut(polygon);

    const std::vector<std::uint32_t>& triangles = earcut.indices;

    // earcut indexes the rings as if concatenated, and so does the vertex
    // buffer: one vertex per ring point, in the same order, and earcut's
    // indices then need only the tile's base added to them.
    const auto base = static_cast<std::uint32_t>(out.size());
    std::uint32_t written = 0;
    for (const auto& ring : polygon)
    {
        for (const mvt::Point& p : ring)
        {
            out.push_back(MapVertex { float(p.x) * sc, float(p.y) * sc, 0.f, 0.f, 0.f, c.r, c.g,
                                      c.b, c.a });
            ++written;
        }
    }

    for (const std::uint32_t i : triangles)
    {
        // earcut has been seen to emit an index past the end on degenerate
        // input; dropping the whole triangle is what keeps a bad ring from
        // painting a wedge across the tile.
        if (i >= written)
        {
            continue;
        }
        indices.push_back(base + i);
    }
}

} // namespace

// Declared in tessellator.h; see the comment there. attributeRef, not
// attribute: this runs per feature and the by-value read costs an allocation
// for every string-typed attribute it touches on the way past.
std::optional<double> attributeNumber(const mvt::Layer& layer, const mvt::Feature& feature,
                                      std::string_view key)
{
    const mvt::Value* value = layer.attributeRef(feature, key);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    if (const auto* d = std::get_if<double>(value))
    {
        return *d;
    }
    if (const auto* i = std::get_if<std::int64_t>(value))
    {
        return double(*i);
    }
    return std::nullopt;
}

std::uint64_t nextSerial()
{
    static std::atomic<std::uint64_t> counter { 1 };
    return counter.fetch_add(1, std::memory_order_relaxed);
}

const char* to_string(MapLayer layer)
{
    switch (layer)
    {
        case MapLayer::Landcover:      return "landcover";
        case MapLayer::Landuse:        return "landuse";
        case MapLayer::Park:           return "park";
        case MapLayer::AerowaySurface: return "aeroway_surface";
        case MapLayer::Water:          return "water";
        case MapLayer::Waterway:       return "waterway";
        case MapLayer::Building:       return "building";
        case MapLayer::AerowayTaxiway: return "aeroway_taxiway";
        case MapLayer::AerowayRunway:  return "aeroway_runway";
        case MapLayer::RoadMinor:      return "road_minor";
        case MapLayer::RoadMajor:      return "road_major";
        case MapLayer::RoadPrimary:    return "road_primary";
        case MapLayer::MotorwayCasing: return "motorway_casing";
        case MapLayer::Motorway:       return "motorway";
        case MapLayer::Rail:           return "rail";
        case MapLayer::RoadBridgeCasing: return "road_bridge_casing";
        case MapLayer::RoadBridge:     return "road_bridge";
        case MapLayer::Boundary:       return "boundary";
        case MapLayer::TrackSurface:   return "track_surface";
        case MapLayer::TrackCentre:    return "track_centre";
    }
    return "unknown";
}

double layerMinZoom(MapLayer layer, const MapStyle_t& style)
{
    switch (layer)
    {
        case MapLayer::Landcover:      return double(style.detail.landcover);
        case MapLayer::Landuse:        return double(style.detail.landuse);
        case MapLayer::Park:           return double(style.detail.park);
        case MapLayer::AerowaySurface: return double(style.detail.aeroway_surface);
        case MapLayer::Water:          return double(style.detail.water);
        case MapLayer::Waterway:       return double(style.detail.waterway);
        case MapLayer::Building:       return double(style.detail.building);
        case MapLayer::AerowayTaxiway: return double(style.detail.aeroway_taxiway);
        case MapLayer::AerowayRunway:  return double(style.detail.aeroway_runway);
        case MapLayer::RoadMinor:      return double(style.detail.road_minor);
        case MapLayer::RoadMajor:      return double(style.detail.road_major);
        case MapLayer::RoadPrimary:    return double(style.detail.road_primary);
        case MapLayer::MotorwayCasing: return double(style.detail.motorway);
        case MapLayer::Motorway:       return double(style.detail.motorway);
        case MapLayer::Rail:           return double(style.detail.rail);
        case MapLayer::RoadBridgeCasing:
        case MapLayer::RoadBridge:     return double(style.detail.bridge);
        case MapLayer::Boundary:       return double(style.detail.boundary);
        case MapLayer::TrackSurface:   return double(style.detail.racetrack_surface);
        case MapLayer::TrackCentre:    return double(style.detail.racetrack_centre);
    }
    return 0.0;
}

float halfWidthFor(MapLayer layer, const MapStyle_t& style)
{
    // Fills carry no width; the shader ignores halfPx when the normal is zero.
    switch (layer)
    {
        case MapLayer::Landcover:
        case MapLayer::Landuse:
        case MapLayer::Park:
        case MapLayer::Water:
        case MapLayer::AerowaySurface:
        case MapLayer::Building:       return 0.0f;
        case MapLayer::Waterway:       return float(style.widths.waterway);
        case MapLayer::AerowayTaxiway: return float(style.widths.aeroway_taxiway);
        case MapLayer::AerowayRunway:  return float(style.widths.aeroway_runway);
        case MapLayer::RoadMinor:      return float(style.widths.road_minor);
        case MapLayer::RoadMajor:      return float(style.widths.road_major);
        case MapLayer::RoadPrimary:    return float(style.widths.road_primary);
        case MapLayer::MotorwayCasing: return float(style.widths.motorway_casing);
        case MapLayer::Motorway:       return float(style.widths.motorway);
        case MapLayer::Rail:           return float(style.widths.rail);
        // Overridden per feature; the layer value only decides whether the
        // layer is skipped outright, and a bridge layer never should be.
        case MapLayer::RoadBridgeCasing:
        case MapLayer::RoadBridge:     return float(style.widths.road_minor);
        case MapLayer::Boundary:       return float(style.widths.boundary);
        // The surface is a FILL -- it is the tarmac, drawn as an area with the
        // infield cut out as a hole -- so it carries no width. The centreline
        // is a line and does.
        case MapLayer::TrackSurface:   return 0.0f;
        case MapLayer::TrackCentre:    return float(style.widths.racetrack_centre);
    }
    return 0.0f;
}

MapLayer roadLayerFor(int priority)
{
    switch (priority)
    {
        case 5:  return MapLayer::Rail;
        case 4:  return MapLayer::Motorway;
        case 3:  return MapLayer::RoadPrimary;
        case 2:  return MapLayer::RoadMajor;
        default: return MapLayer::RoadMinor;
    }
}

int roadPriority(std::string_view className)
{
    if (className == "motorway")
    {
        return 4;
    }
    if (className == "trunk" || className == "primary")
    {
        return 3;
    }
    if (className == "secondary" || className == "tertiary")
    {
        return 2;
    }
    if (className == "rail" || className == "transit")
    {
        return 5;
    }
    // Everything else -- minor, service, track, and any class this build has
    // never heard of -- draws as a minor road. A hole in the road network is
    // worse than a road of the wrong width.
    return 1;
}

float widthScaleForZoom(double zoom)
{
    // Roads keep roughly their real width on the ground rather than becoming
    // hairlines at z14 and stripes at z8. One scale for every layer, applied in
    // the shader, so a zoom change never invalidates a tessellation.
    const double t = std::clamp((zoom - 5.0) / 9.0, 0.0, 1.0);
    return float(0.15 + (0.85 * t));
}

TileGeometry tessellate(const mvt::Tile& tile, const MapStyle_t& style)
{
    TileGeometry out;
    // A z14 city tile is ~70k vertices once expanded; reserving saves a dozen
    // reallocations of a growing megabyte.
    out.vertices.reserve(1 << 15);
    out.indices.reserve(1 << 16);

    // Reused across every polygon in the tile: cleared, never reallocated.
    std::vector<std::span<const mvt::Point>> polygon;

    for (std::size_t li = 0; li < kMapLayerCount; ++li)
    {
        out.layerStart[li] = static_cast<std::uint32_t>(out.vertices.size());
        out.layerIndexStart[li] = static_cast<std::uint32_t>(out.indices.size());

        const LayerSpec& spec = kSpecs[li];
        if (!layerEnabled(spec.layer, style))
        {
            continue;
        }

        const mvt::Layer* layer = tile.layer(spec.sourceLayer);
        if (layer == nullptr || layer->extent == 0)
        {
            continue;
        }

        // Tile-local: [0, extent] becomes [0, 1]. Independent of zoom, camera
        // and neighbours, which is what makes the result cacheable.
        const float sc = 1.0f / float(layer->extent);
        const Colour colour = colourFor(spec.layer, style);
        // NOT scaled by style.road_width_scale here. That multiplier rides the
        // widthScale uniform, which is what lets a width change be a uniform
        // write rather than a re-tessellation of the city. Applying it in both
        // places squares it, and a scale of 2 draws roads four times too wide.
        const float halfPx = halfWidthFor(spec.layer, style);

        // A line with no width is the documented way to drop a layer without
        // touching the archive. Skipping it here rather than emitting
        // zero-area triangles is what makes that free: no vertices, no upload,
        // no draw call.
        if (!spec.fill && halfPx <= 0.0f && spec.featureHalfWidth == nullptr)
        {
            continue;
        }

        // Roads are the only thing a client joins back to the graph on, so they
        // are the only thing whose ranges are worth recording. See FeatureRange.
        const bool trackRanges = std::string_view(spec.sourceLayer) == "transportation";

        for (const mvt::Feature& feature : layer->features)
        {
            if (!accepts(spec, *layer, feature))
            {
                continue;
            }

            const auto rangeStart = static_cast<std::uint32_t>(out.indices.size());

            switch (feature.type)
            {
                case mvt::GeomType::Polygon:
                {
                    if (!spec.fill)
                    {
                        break;
                    }
                    // Regroup flat rings into polygons: a new exterior ring
                    // starts a new polygon, everything after it is a hole.
                    // VIEWS, not copies. This used to be a
                    // vector<vector<Point>> that each ring was push_back'ed
                    // into: one allocation and a full point copy per ring, per
                    // feature, on 214k polygons a tile-set. earcut only ever
                    // reads the rings, and nth<> already teaches it to read an
                    // mvt::Point directly, so a span is all it needs.
                    polygon.clear();
                    for (const auto& ring : feature.rings)
                    {
                        if (ring.size() < 3)
                        {
                            continue;
                        }
                        if (mvt::isExteriorRing(ring) && !polygon.empty())
                        {
                            emitPolygon(out.vertices, out.indices, polygon, sc, colour);
                            polygon.clear();
                        }
                        polygon.emplace_back(ring.data(), ring.size());
                    }
                    if (!polygon.empty())
                    {
                        emitPolygon(out.vertices, out.indices, polygon, sc, colour);
                    }
                    break;
                }

                case mvt::GeomType::LineString:
                {
                    if (spec.fill)
                    {
                        break;
                    }
                    // Per-feature weight and colour where the layer asks for
                    // them. `widths.*` and the palette stay the one place
                    // either is set -- these hooks pick WHICH entry, they do
                    // not invent values -- so a width of zero still hides its
                    // roads, on a bridge as at grade.
                    const float featureHalfPx =
                        spec.featureHalfWidth != nullptr
                            ? spec.featureHalfWidth(*layer, feature, style)
                            : halfPx;
                    if (featureHalfPx <= 0.0f)
                    {
                        break;
                    }
                    const Colour featureColour =
                        spec.featureColour != nullptr ? spec.featureColour(*layer, feature, style)
                                                      : colour;
                    for (const auto& ring : feature.rings)
                    {
                        emitPolyline(out.vertices, out.indices, ring, sc, featureHalfPx,
                                     featureColour);
                    }
                    break;
                }

                case mvt::GeomType::Point:
                case mvt::GeomType::Unknown:
                    // Points are labels, and labels are a QPainter pass -- they
                    // must stay upright while the map turns.
                    break;
            }

            if (trackRanges && feature.hasId)
            {
                const auto written = static_cast<std::uint32_t>(out.indices.size()) - rangeStart;
                if (written != 0)
                {
                    out.roads.push_back(FeatureRange { feature.id, rangeStart, written });
                }
            }
        }
    }

    // Sorted so a lookup is a binary search rather than a scan of a few
    // thousand roads, on every frame, per highlighted id.
    std::sort(out.roads.begin(), out.roads.end(),
              [](const FeatureRange& a, const FeatureRange& b) {
                  return a.osmWayId < b.osmWayId;
              });

    out.layerStart[kMapLayerCount] = static_cast<std::uint32_t>(out.vertices.size());
    out.layerIndexStart[kMapLayerCount] = static_cast<std::uint32_t>(out.indices.size());
    return out;
}

} // namespace map_widget
