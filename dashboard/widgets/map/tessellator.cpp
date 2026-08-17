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

// Which source layer and class each of our draw layers is cut from. STRUCTURE
// only -- widths and zoom thresholds are the user's, and live in MapStyle_t so
// there is exactly one place their defaults are written down.
struct LayerSpec
{
    MapLayer layer;
    const char* sourceLayer;
    int roadClass;      // 0 when the source layer is the whole filter
    bool fill;
};

constexpr std::array<LayerSpec, kMapLayerCount> kSpecs { {
    { MapLayer::Landcover,      "landcover",      0, true  },
    { MapLayer::Landuse,        "landuse",        0, true  },
    { MapLayer::Park,           "park",           0, true  },
    { MapLayer::Water,          "water",          0, true  },
    { MapLayer::Waterway,       "waterway",       0, false },
    { MapLayer::Building,       "building",       0, true  },
    { MapLayer::RoadMinor,      "transportation", 1, false },
    { MapLayer::RoadMajor,      "transportation", 2, false },
    { MapLayer::RoadPrimary,    "transportation", 3, false },
    { MapLayer::MotorwayCasing, "transportation", 4, false },
    { MapLayer::Motorway,       "transportation", 4, false },
    { MapLayer::Rail,           "transportation", 5, false },
    { MapLayer::Boundary,       "boundary",       0, false },
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
        case MapLayer::Water:          return toRgba(style.water);
        case MapLayer::Waterway:       return toRgba(style.waterway);
        case MapLayer::Building:       return toRgba(style.building);
        case MapLayer::RoadMinor:      return toRgba(style.road_minor);
        case MapLayer::RoadMajor:      return toRgba(style.road_major);
        case MapLayer::RoadPrimary:    return toRgba(style.road_primary);
        case MapLayer::MotorwayCasing: return toRgba(style.motorway_casing);
        case MapLayer::Motorway:       return toRgba(style.motorway);
        case MapLayer::Rail:           return toRgba(style.rail);
        case MapLayer::Boundary:       return toRgba(style.boundary);
        case MapLayer::TrackSurface:   return toRgba(style.racetrack_surface);
        case MapLayer::TrackCentre:    return toRgba(style.racetrack_centre);
    }
    return { 1.f, 0.f, 1.f, 1.f };
}

bool layerEnabled(MapLayer layer, const MapStyle_t& style)
{
    switch (layer)
    {
        case MapLayer::Building: return style.show_buildings;
        case MapLayer::Boundary: return style.show_boundaries;
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
    if (spec.roadClass == 0)
    {
        return true;
    }
    return roadPriority(layer.attributeText(feature, "class")) == spec.roadClass;
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

void emitPolyline(std::vector<MapVertex>& out, const std::vector<mvt::Point>& ring, float sc,
                  float halfPx, const Colour& c)
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

    for (std::size_t i = 0; i + 1 < m; ++i)
    {
        const auto& p0 = pts[i];
        const auto& p1 = pts[i + 1];
        const auto& n0 = normal[i];
        const auto& n1 = normal[i + 1];

        const MapVertex a { p0.first, p0.second,  n0.first,  n0.second, halfPx, c.r, c.g, c.b, c.a };
        const MapVertex b { p0.first, p0.second, -n0.first, -n0.second, halfPx, c.r, c.g, c.b, c.a };
        const MapVertex d { p1.first, p1.second,  n1.first,  n1.second, halfPx, c.r, c.g, c.b, c.a };
        const MapVertex e { p1.first, p1.second, -n1.first, -n1.second, halfPx, c.r, c.g, c.b, c.a };

        out.push_back(a); out.push_back(b); out.push_back(d);
        out.push_back(d); out.push_back(b); out.push_back(e);
    }
}

// ----------------------------------------------------------------- polygons

// MVT hands rings out flat: an exterior ring, then its holes, then the next
// exterior ring. Winding is the ONLY thing that says which is which, so the
// rings have to be regrouped into polygons before they can be triangulated.
void emitPolygon(std::vector<MapVertex>& out,
                 const std::vector<std::vector<mvt::Point>>& polygon, float sc, const Colour& c)
{
    if (polygon.empty() || polygon.front().size() < 3)
    {
        return;
    }

    const std::vector<std::uint32_t> indices = mapbox::earcut<std::uint32_t>(polygon);

    // earcut indexes the rings as if concatenated, so flatten in the same order.
    std::vector<const mvt::Point*> flat;
    for (const auto& ring : polygon)
    {
        for (const mvt::Point& p : ring)
        {
            flat.push_back(&p);
        }
    }

    for (const std::uint32_t i : indices)
    {
        if (i >= flat.size())
        {
            continue;
        }
        out.push_back(MapVertex { float(flat[i]->x) * sc, float(flat[i]->y) * sc, 0.f, 0.f, 0.f,
                                  c.r, c.g, c.b, c.a });
    }
}

} // namespace

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
        case MapLayer::Water:          return "water";
        case MapLayer::Waterway:       return "waterway";
        case MapLayer::Building:       return "building";
        case MapLayer::RoadMinor:      return "road_minor";
        case MapLayer::RoadMajor:      return "road_major";
        case MapLayer::RoadPrimary:    return "road_primary";
        case MapLayer::MotorwayCasing: return "motorway_casing";
        case MapLayer::Motorway:       return "motorway";
        case MapLayer::Rail:           return "rail";
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
        case MapLayer::Water:          return double(style.detail.water);
        case MapLayer::Waterway:       return double(style.detail.waterway);
        case MapLayer::Building:       return double(style.detail.building);
        case MapLayer::RoadMinor:      return double(style.detail.road_minor);
        case MapLayer::RoadMajor:      return double(style.detail.road_major);
        case MapLayer::RoadPrimary:    return double(style.detail.road_primary);
        case MapLayer::MotorwayCasing: return double(style.detail.motorway);
        case MapLayer::Motorway:       return double(style.detail.motorway);
        case MapLayer::Rail:           return double(style.detail.rail);
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
        case MapLayer::Building:       return 0.0f;
        case MapLayer::Waterway:       return float(style.widths.waterway);
        case MapLayer::RoadMinor:      return float(style.widths.road_minor);
        case MapLayer::RoadMajor:      return float(style.widths.road_major);
        case MapLayer::RoadPrimary:    return float(style.widths.road_primary);
        case MapLayer::MotorwayCasing: return float(style.widths.motorway_casing);
        case MapLayer::Motorway:       return float(style.widths.motorway);
        case MapLayer::Rail:           return float(style.widths.rail);
        case MapLayer::Boundary:       return float(style.widths.boundary);
        // The surface is a FILL -- it is the tarmac, drawn as an area with the
        // infield cut out as a hole -- so it carries no width. The centreline
        // is a line and does.
        case MapLayer::TrackSurface:   return 0.0f;
        case MapLayer::TrackCentre:    return float(style.widths.racetrack_centre);
    }
    return 0.0f;
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
    out.vertices.reserve(1 << 16);

    for (std::size_t li = 0; li < kMapLayerCount; ++li)
    {
        out.layerStart[li] = static_cast<std::uint32_t>(out.vertices.size());

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
        if (!spec.fill && halfPx <= 0.0f)
        {
            continue;
        }

        for (const mvt::Feature& feature : layer->features)
        {
            if (!accepts(spec, *layer, feature))
            {
                continue;
            }

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
                    std::vector<std::vector<mvt::Point>> polygon;
                    for (const auto& ring : feature.rings)
                    {
                        if (ring.size() < 3)
                        {
                            continue;
                        }
                        if (mvt::isExteriorRing(ring) && !polygon.empty())
                        {
                            emitPolygon(out.vertices, polygon, sc, colour);
                            polygon.clear();
                        }
                        polygon.push_back(ring);
                    }
                    if (!polygon.empty())
                    {
                        emitPolygon(out.vertices, polygon, sc, colour);
                    }
                    break;
                }

                case mvt::GeomType::LineString:
                {
                    if (spec.fill)
                    {
                        break;
                    }
                    for (const auto& ring : feature.rings)
                    {
                        emitPolyline(out.vertices, ring, sc, halfPx, colour);
                    }
                    break;
                }

                case mvt::GeomType::Point:
                case mvt::GeomType::Unknown:
                    // Points are labels, and labels are a QPainter pass -- they
                    // must stay upright while the map turns.
                    break;
            }
        }
    }

    out.layerStart[kMapLayerCount] = static_cast<std::uint32_t>(out.vertices.size());
    return out;
}

} // namespace map_widget
