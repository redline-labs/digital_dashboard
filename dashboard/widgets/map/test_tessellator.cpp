// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vector tiles into triangles.
//
// This is where the geometry is actually decided -- the GPU only draws what
// comes out of here -- and every mistake it can make renders rather than fails:
// a hole filled in solid, a road with a notch at every corner, a spike across a
// park. So it gets a Qt-free `unit` test rather than being left to a screenshot
// of terrain nobody has seen before.
//
// Tiles are hand-built rather than pulled from the archive, so the expected
// vertex counts are ones a human worked out. libs/mvt's real-tile test is the
// other half of that argument.

#include "map/tessellator.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>
#include <memory>
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

using map_widget::kMapLayerCount;
using map_widget::MapLayer;
using map_widget::MapVertex;
using map_widget::tessellate;

// --- tiny mvt::Tile builders ------------------------------------------------

mvt::Feature polygon(std::vector<std::vector<mvt::Point>> rings)
{
    mvt::Feature f;
    f.type = mvt::GeomType::Polygon;
    f.rings = std::move(rings);
    return f;
}

mvt::Feature line(std::vector<mvt::Point> points)
{
    mvt::Feature f;
    f.type = mvt::GeomType::LineString;
    f.rings.push_back(std::move(points));
    return f;
}

mvt::Tile tileWith(const std::string& layerName, std::vector<mvt::Feature> features,
                   std::uint32_t extent = 4096)
{
    mvt::Layer layer;
    layer.name = layerName;
    layer.extent = extent;
    layer.features = std::move(features);
    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));
    return tile;
}

// A road tile needs a `class` attribute for the layer split to work.
mvt::Tile roadTile(const std::string& className, std::vector<mvt::Point> points)
{
    mvt::Layer layer;
    layer.name = "transportation";
    layer.extent = 4096;
    layer.keys = { "class" };
    layer.values = { mvt::Value(std::in_place_type<std::string>, className) };
    mvt::Feature f = line(std::move(points));
    f.tags = { 0, 0 };
    layer.features.push_back(std::move(f));
    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));
    return tile;
}


// An aeroway tile, like roadTile but for the layer that carries runways,
// taxiways and aprons under an open `class` vocabulary.
mvt::Tile aerowayTile(const std::string& className, mvt::Feature feature)
{
    mvt::Layer layer;
    layer.name = "aeroway";
    layer.extent = 4096;
    layer.keys = { "class" };
    layer.values = { mvt::Value(std::in_place_type<std::string>, className) };
    feature.tags = { 0, 0 };
    layer.features.push_back(std::move(feature));
    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));
    return tile;
}


// A boundary tile carrying OSM's own admin_level as a NUMBER, which is how
// map_build writes it.
mvt::Tile boundaryTile(std::int64_t adminLevel, bool writeLevel = true)
{
    mvt::Layer layer;
    layer.name = "boundary";
    layer.extent = 4096;
    mvt::Feature f = line({ { 0, 0 }, { 4096, 0 } });
    if (writeLevel)
    {
        layer.keys = { "admin_level" };
        layer.values = { mvt::Value(adminLevel) };
        f.tags = { 0, 0 };
    }
    layer.features.push_back(std::move(f));
    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));
    return tile;
}

// The widest |normal| in a layer, which is what the shader multiplies by
// halfPx -- so this is the on-screen weight of the line.
float halfPxOf(const map_widget::TileGeometry& geometry, MapLayer layer)
{
    const auto first = geometry.layerStart[static_cast<std::size_t>(layer)];
    const auto last = geometry.layerStart[static_cast<std::size_t>(layer) + 1];
    float widest = 0.0f;
    for (auto i = first; i < last; ++i)
    {
        widest = std::max(widest, geometry.vertices[i].halfPx);
    }
    return widest;
}

// A transportation tile carrying one road of `className`, optionally on a
// bridge. This is the shape the overpass problem lives in.
mvt::Tile roadTileBrunnel(const std::string& className, const std::string& brunnel,
                          std::vector<mvt::Point> points)
{
    mvt::Layer layer;
    layer.name = "transportation";
    layer.extent = 4096;
    layer.keys = { "class" };
    layer.values = { mvt::Value(std::in_place_type<std::string>, className) };
    mvt::Feature f = line(std::move(points));
    f.tags = { 0, 0 };
    if (!brunnel.empty())
    {
        layer.keys.push_back("brunnel");
        layer.values.push_back(mvt::Value(std::in_place_type<std::string>, brunnel));
        f.tags.push_back(1);
        f.tags.push_back(1);
    }
    layer.features.push_back(std::move(f));
    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));
    return tile;
}

// Clockwise in screen coords (y down) = positive area = exterior ring.
std::vector<mvt::Point> square(int x, int y, int size)
{
    return { { x, y }, { x + size, y }, { x + size, y + size }, { x, y + size } };
}

std::vector<mvt::Point> squareReversed(int x, int y, int size)
{
    return { { x, y }, { x, y + size }, { x + size, y + size }, { x + size, y } };
}

// ============================================================================
// Layer routing
// ============================================================================

void test_road_classes_reach_the_right_layers()
{
    using map_widget::roadPriority;

    check(roadPriority("motorway") == 4, "motorway is the top road class");
    check(roadPriority("trunk") == 3 && roadPriority("primary") == 3, "trunk and primary");
    check(roadPriority("secondary") == 2 && roadPriority("tertiary") == 2,
          "secondary and tertiary");
    check(roadPriority("rail") == 5, "rail is its own layer, not a road");
    check(roadPriority("minor") == 1, "minor");

    // An unrecognised class still draws. A hole in the road network is worse
    // than a road of the wrong width.
    check(roadPriority("a_class_from_the_future") == 1, "an unknown class draws as a minor road");
    check(roadPriority("") == 1, "as does one with no class at all");
}

void test_a_motorway_lands_in_both_casing_and_fill()
{
    // A motorway is drawn twice -- a wide dark casing, then a narrower bright
    // fill on top -- which is what makes it read as one road rather than a
    // stripe. If either layer were empty it would look like an ordinary road.
    const MapStyle_t style;
    const auto geometry = tessellate(roadTile("motorway", { { 0, 0 }, { 4096, 4096 } }), style);

    check(geometry.layerVertexCount(MapLayer::MotorwayCasing) > 0, "the casing has geometry");
    check(geometry.layerVertexCount(MapLayer::Motorway) > 0, "and so does the fill");
    check(geometry.layerVertexCount(MapLayer::RoadMinor) == 0,
          "and it does not also draw as a minor road");
}

void test_layers_are_contiguous_and_ordered()
{
    // The draw loop slices the vertex buffer with layerStart, so the offsets
    // have to be non-decreasing and end at the vertex count. A wrong offset
    // draws one layer's triangles with another layer's colours.
    const MapStyle_t style;
    const auto geometry = tessellate(roadTile("primary", { { 0, 0 }, { 100, 100 }, { 200, 0 } }),
                                     style);

    for (std::size_t i = 0; i < kMapLayerCount; ++i)
    {
        check(geometry.layerStart[i] <= geometry.layerStart[i + 1],
              "layer " + std::to_string(i) + " starts before it ends");
    }
    check(geometry.layerStart[kMapLayerCount] == geometry.vertices.size(),
          "the last offset is the vertex count");
}

// ============================================================================
// Polygons
// ============================================================================

void test_a_square_becomes_two_triangles()
{
    const MapStyle_t style;
    const auto geometry = tessellate(tileWith("water", { polygon({ square(0, 0, 1000) }) }), style);

    // A quad is two triangles: FOUR vertices, drawn by six indices. The corners
    // are shared, which is the whole point of indexing -- writing them out cost
    // six vertices for the same two triangles.
    check(geometry.layerVertexCount(MapLayer::Water) == 4,
          "a 4-point ring keeps its 4 corners, got " +
              std::to_string(geometry.layerVertexCount(MapLayer::Water)));
    check(geometry.layerIndexCount(MapLayer::Water) == 6,
          "and triangulates to 2 triangles, got " +
              std::to_string(geometry.layerIndexCount(MapLayer::Water) / 3));
}

void test_a_hole_is_cut_rather_than_filled()
{
    // THE polygon trap. MVT hands rings out flat -- exterior, then its holes --
    // and winding is the only thing that says which is which. Fan triangulating
    // each ring independently fills the hole in solid, so every lake with an
    // island becomes a solid lake.
    const MapStyle_t style;
    const auto withHole = tessellate(
        tileWith("water", { polygon({ square(0, 0, 1000), squareReversed(250, 250, 500) }) }),
        style);
    const auto solid =
        tessellate(tileWith("water", { polygon({ square(0, 0, 1000) }) }), style);

    check(withHole.layerIndexCount(MapLayer::Water) > solid.layerIndexCount(MapLayer::Water),
          "a ring with a hole needs MORE triangles than one without -- the hole is cut around, "
          "not filled");

    // And no triangle may have its centroid inside the hole. Walked through the
    // INDICES now: the vertices are the ring's corners and are no longer in
    // triangle order.
    const auto& v = withHole.vertices;
    const auto& idx = withHole.indices;
    int inside = 0;
    for (std::size_t i = 0; i + 2 < idx.size(); i += 3)
    {
        const float cx = (v[idx[i]].x + v[idx[i + 1]].x + v[idx[i + 2]].x) / 3.0f;
        const float cy = (v[idx[i]].y + v[idx[i + 1]].y + v[idx[i + 2]].y) / 3.0f;
        // Tile-local coordinates: extent 4096, hole spans 250..750.
        if (cx > 250.0f / 4096.0f && cx < 750.0f / 4096.0f && cy > 250.0f / 4096.0f &&
            cy < 750.0f / 4096.0f)
        {
            ++inside;
        }
    }
    check(inside == 0, "no triangle falls inside the hole, got " + std::to_string(inside));
}

void test_two_polygons_in_one_feature_stay_separate()
{
    // Exterior, hole, exterior: the second exterior ring starts a new polygon
    // rather than becoming a hole of the first.
    const MapStyle_t style;
    const auto geometry = tessellate(
        tileWith("water", { polygon({ square(0, 0, 800), squareReversed(200, 200, 400),
                                      square(2000, 2000, 800) }) }),
        style);

    check(geometry.layerVertexCount(MapLayer::Water) > 0, "both polygons produce geometry");

    // The far polygon must be present -- if the regrouping were wrong it would
    // have been treated as a hole of the first and dropped.
    bool sawFar = false;
    for (const MapVertex& vertex : geometry.vertices)
    {
        if (vertex.x > 2000.0f / 4096.0f)
        {
            sawFar = true;
        }
    }
    check(sawFar, "the second exterior ring is its own polygon, not a hole of the first");
}

void test_degenerate_rings_are_dropped()
{
    const MapStyle_t style;
    const auto twoPoint =
        tessellate(tileWith("water", { polygon({ { { 0, 0 }, { 10, 10 } } }) }), style);
    check(twoPoint.layerVertexCount(MapLayer::Water) == 0, "a 2-point ring bounds no area");

    const auto empty = tessellate(tileWith("water", { polygon({}) }), style);
    check(empty.layerVertexCount(MapLayer::Water) == 0, "an empty ring produces nothing");
}

// ============================================================================
// Polylines and joins
// ============================================================================

void test_a_two_point_line_is_one_quad()
{
    const MapStyle_t style;
    const auto geometry = tessellate(roadTile("primary", { { 0, 0 }, { 1000, 0 } }), style);
    // Two points, one either side of the centreline each: FOUR vertices, six
    // indices. The expanded form wrote six vertices for the same quad.
    check(geometry.layerVertexCount(MapLayer::RoadPrimary) == 4,
          "one segment is four vertices, got " +
              std::to_string(geometry.layerVertexCount(MapLayer::RoadPrimary)));
    check(geometry.layerIndexCount(MapLayer::RoadPrimary) == 6,
          "drawn as one quad, got " +
              std::to_string(geometry.layerIndexCount(MapLayer::RoadPrimary) / 3) + " triangles");
}

void test_a_corner_shares_one_normal_so_there_is_no_notch()
{
    // The mitre. With an independent quad per segment the two quads meet at a
    // point and leave a wedge of background showing at every corner -- at z14
    // that is thousands of notches. Sharing one mitred normal at the shared
    // point closes them.
    const MapStyle_t style;
    // A right angle: east, then south.
    const auto geometry =
        tessellate(roadTile("primary", { { 0, 0 }, { 1000, 0 }, { 1000, 1000 } }), style);

    const auto& v = geometry.vertices;
    const std::uint32_t start = geometry.layerStart[std::size_t(MapLayer::RoadPrimary)];
    const std::uint32_t count = geometry.layerVertexCount(MapLayer::RoadPrimary);
    // Three points, two vertices each. Indexing makes the shared normal
    // literal: the corner is now ONE pair of vertices used by both segments,
    // where the expanded form wrote it twice and relied on the two copies
    // agreeing.
    check(count == 6, "three points are six vertices, got " + std::to_string(count));
    check(geometry.layerIndexCount(MapLayer::RoadPrimary) == 12, "and two quads");
    if (count != 6)
    {
        return;
    }

    // The middle point's pair, which both segments index.
    const MapVertex& corner = v[start + 2];

    // A 90-degree turn mitres to 1/cos(45) = sqrt(2) times unit length.
    const float mitreLength = std::sqrt((corner.nx * corner.nx) + (corner.ny * corner.ny));
    check(std::abs(mitreLength - std::sqrt(2.0f)) < 1e-3f,
          "the mitre is extended by 1/cos(half-angle), got " + std::to_string(mitreLength));
}

void test_a_hairpin_bevels_rather_than_growing_a_spike()
{
    // As a turn approaches 180 degrees the mitre length runs to infinity. A
    // switchback with an unbounded mitre grows a spike hundreds of pixels long
    // across the map.
    const MapStyle_t style;
    const auto geometry =
        tessellate(roadTile("primary", { { 0, 0 }, { 1000, 0 }, { 5, 1 } }), style);

    const std::uint32_t start = geometry.layerStart[std::size_t(MapLayer::RoadPrimary)];
    const std::uint32_t count = geometry.layerVertexCount(MapLayer::RoadPrimary);
    check(count > 0, "the hairpin produces geometry");
    if (count == 0)
    {
        return;
    }

    float longest = 0.0f;
    for (std::uint32_t i = start; i < start + count; ++i)
    {
        const MapVertex& vertex = geometry.vertices[i];
        longest = std::max(longest,
                           std::sqrt((vertex.nx * vertex.nx) + (vertex.ny * vertex.ny)));
    }
    check(longest <= 4.01f, "no normal exceeds the mitre limit, got " + std::to_string(longest));
}

void test_repeated_points_do_not_produce_nan()
{
    // A zero-length segment has no direction, so its normal is 0/0. One NaN
    // vertex takes out the whole triangle it belongs to, and real tiles do
    // contain repeated points.
    const MapStyle_t style;
    const auto geometry = tessellate(
        roadTile("primary", { { 100, 100 }, { 100, 100 }, { 500, 100 }, { 500, 100 } }), style);

    for (const MapVertex& vertex : geometry.vertices)
    {
        check(std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.nx) &&
                  std::isfinite(vertex.ny),
              "no vertex is NaN after a repeated point");
    }
    check(geometry.layerVertexCount(MapLayer::RoadPrimary) == 4,
          "and the duplicate points collapse to one segment, got " +
              std::to_string(geometry.layerVertexCount(MapLayer::RoadPrimary) / 2) + " points");
    check(geometry.layerIndexCount(MapLayer::RoadPrimary) == 6, "drawn as a single quad");
}

// ============================================================================
// Coordinates and style
// ============================================================================

void test_coordinates_are_tile_local_and_extent_normalised()
{
    // Vertices must be independent of zoom, camera and neighbours -- that is
    // what makes a tessellation survive a pan. And the extent is per layer, so
    // normalising by a hardcoded 4096 would draw a 512-extent layer 8x too big.
    const MapStyle_t style;
    const auto normal = tessellate(tileWith("water", { polygon({ square(0, 0, 4096) }) }, 4096),
                                   style);
    const auto small = tessellate(tileWith("water", { polygon({ square(0, 0, 512) }) }, 512),
                                  style);

    const auto maxX = [](const auto& g) {
        float m = 0.0f;
        for (const MapVertex& v : g.vertices) m = std::max(m, v.x);
        return m;
    };

    check(std::abs(maxX(normal) - 1.0f) < 1e-5f, "a full-extent ring reaches local x = 1");
    check(std::abs(maxX(small) - 1.0f) < 1e-5f,
          "and so does one in a layer with a different extent");
}

void test_style_switches_are_honoured()
{
    MapStyle_t style;
    const auto buildings = tileWith("building", { polygon({ square(0, 0, 500) }) });

    check(tessellate(buildings, style).layerVertexCount(MapLayer::Building) > 0,
          "buildings draw by default");

    style.show_buildings = false;
    check(tessellate(buildings, style).layerVertexCount(MapLayer::Building) == 0,
          "and not when switched off");
}

void test_per_layer_widths_come_from_the_style()
{
    using map_widget::halfWidthFor;

    MapStyle_t style;
    check(halfWidthFor(MapLayer::Motorway, style) > halfWidthFor(MapLayer::RoadMinor, style),
          "a motorway is wider than a residential street by default");
    check(halfWidthFor(MapLayer::MotorwayCasing, style) > halfWidthFor(MapLayer::Motorway, style),
          "and the casing is wider than the fill it sits under -- otherwise it never shows");
    check(halfWidthFor(MapLayer::Water, style) == 0.0f, "a fill carries no width");

    // Retuning one road must not move another. A single global multiplier could
    // not express "make motorways prominent", which is the whole reason the
    // widths are per layer.
    const float minorBefore = halfWidthFor(MapLayer::RoadMinor, style);
    style.widths.motorway = 12.0;
    check(halfWidthFor(MapLayer::Motorway, style) == 12.0f, "the style's width is what is used");
    check(halfWidthFor(MapLayer::RoadMinor, style) == minorBefore, "and its neighbours do not move");

    // Zero is a documented way to drop a layer without touching the archive.
    style.widths.rail = 0.0;
    const auto geometry = tessellate(roadTile("rail", { { 0, 0 }, { 1000, 0 } }), style);
    check(geometry.layerVertexCount(MapLayer::Rail) == 0, "a width of zero draws nothing at all");
}

void test_road_width_scale_is_applied_exactly_once()
{
    // A REGRESSION TEST, and a cheap one. road_width_scale rides the shader
    // uniform so that widening every road is a uniform write rather than a
    // re-tessellation. Baking it into the vertices as well squares it, and a
    // scale of 2 silently draws roads four times too wide -- which reads as
    // "the widths are wrong" rather than as a double-apply.
    MapStyle_t style;
    const auto tile = roadTile("primary", { { 0, 0 }, { 1000, 0 } });

    const auto atOne = tessellate(tile, style);
    style.road_width_scale = 4.0;
    const auto atFour = tessellate(tile, style);

    const std::uint32_t start = atOne.layerStart[std::size_t(MapLayer::RoadPrimary)];
    check(atOne.vertices.size() == atFour.vertices.size(), "the scale changes no geometry");
    check(atOne.vertices[start].halfPx == atFour.vertices[start].halfPx,
          "and does not touch the baked half-width either -- it belongs to the uniform alone");
    check(atOne.vertices[start].halfPx == float(style.widths.road_primary),
          "which is the style's width, unscaled");
}

void test_detail_thresholds_come_from_the_style()
{
    using map_widget::layerMinZoom;

    MapStyle_t style;
    check(layerMinZoom(MapLayer::Building, style) == 13.0, "buildings default to the archive's z13");
    check(layerMinZoom(MapLayer::Motorway, style) == 5.0, "and motorways to z5");
    check(layerMinZoom(MapLayer::MotorwayCasing, style) == layerMinZoom(MapLayer::Motorway, style),
          "a motorway and its casing appear together, or the casing draws alone");

    style.detail.building = 15;
    check(layerMinZoom(MapLayer::Building, style) == 15.0, "raising the threshold is honoured");
    check(layerMinZoom(MapLayer::Motorway, style) == 5.0, "without disturbing the others");
}

void test_width_scale_tapers_with_zoom()
{
    using map_widget::widthScaleForZoom;

    check(widthScaleForZoom(14.0) > 0.99f, "full width at z14");
    check(widthScaleForZoom(5.0) < 0.2f, "much thinner at z5");
    check(widthScaleForZoom(9.5) > widthScaleForZoom(8.0), "and monotonic between");
    // Clamped, or a camera outside the archive's range produces a negative
    // width and roads that vanish.
    check(widthScaleForZoom(0.0) > 0.0f, "clamped below");
    check(widthScaleForZoom(22.0) <= 1.0f, "and above");
}

void test_every_geometry_gets_its_own_serial()
{
    // The renderer decides whether to re-upload by comparing serials, and it
    // does that BECAUSE addresses are not safe: geometry freed by a cache
    // eviction is routinely handed straight back by the allocator, so a
    // replacement tile can land on the dead one's address. The serial has to
    // stay unique even then -- which is what the middle of this test pins.
    const std::uint64_t first = map_widget::TileGeometry {}.serial;
    const std::uint64_t second = map_widget::TileGeometry {}.serial;
    check(first != second, "two geometries never share a serial");
    check(second > first, "and serials only go up");

    // Allocate, free, allocate. On most allocators the second heap object gets
    // the first one's address; its serial must still differ.
    std::uintptr_t deadAddress = 0;
    {
        auto dead = std::make_unique<map_widget::TileGeometry>();
        deadAddress = reinterpret_cast<std::uintptr_t>(dead.get());
    }
    auto reborn = std::make_unique<map_widget::TileGeometry>();
    if (reinterpret_cast<std::uintptr_t>(reborn.get()) == deadAddress)
    {
        SPDLOG_INFO("the allocator reused the freed address, as expected");
    }
    check(reborn->serial > second, "a reused address still yields a fresh serial");

    // And tessellate() hands out a serial too -- a returned-by-value geometry
    // must not carry a default or duplicated one.
    const MapStyle_t style;
    const auto a = tessellate(tileWith("water", { polygon({ square(0, 0, 100) }) }), style);
    const auto b = tessellate(tileWith("water", { polygon({ square(0, 0, 100) }) }), style);
    check(a.serial != 0 && a.serial != b.serial,
          "two tessellations of the same tile are still distinguishable");
}

void test_an_empty_tile_yields_nothing_but_valid_offsets()
{
    const MapStyle_t style;
    const auto geometry = tessellate(mvt::Tile {}, style);
    check(geometry.vertices.empty(), "an empty tile produces no vertices");
    check(geometry.layerStart[kMapLayerCount] == 0, "and offsets that still add up");
}

} // namespace


// ============================================================================
// Aeroway
// ============================================================================

// The whole point of splitting the layer: a runway and a taxiway share one
// source layer and one class key, and must not share a width. Drawn at one
// width, either the runway stops reading as the landmark that identifies an
// airport, or every taxiway becomes a second runway.
void test_a_runway_and_a_taxiway_land_in_different_layers()
{
    const MapStyle_t style;

    const auto runway = tessellate(aerowayTile("runway", line({ { 0, 0 }, { 4096, 4096 } })), style);
    check(runway.layerVertexCount(MapLayer::AerowayRunway) > 0, "a runway reaches the runway layer");
    check(runway.layerVertexCount(MapLayer::AerowayTaxiway) == 0,
          "and not the taxiway layer");

    const auto taxiway = tessellate(aerowayTile("taxiway", line({ { 0, 0 }, { 4096, 4096 } })), style);
    check(taxiway.layerVertexCount(MapLayer::AerowayTaxiway) > 0,
          "a taxiway reaches the taxiway layer");
    check(taxiway.layerVertexCount(MapLayer::AerowayRunway) == 0, "and not the runway layer");

    // `aeroway` is an open vocabulary -- map_rules passes the OSM value
    // through -- so a class nobody listed has to go somewhere rather than
    // vanish, and the narrow layer is the safe direction to be wrong in.
    const auto other = tessellate(aerowayTile("a_class_from_the_future",
                                              line({ { 0, 0 }, { 4096, 4096 } })), style);
    check(other.layerVertexCount(MapLayer::AerowayTaxiway) > 0,
          "an unrecognised aeroway class draws at taxiway width rather than vanishing");
    check(other.layerVertexCount(MapLayer::AerowayRunway) == 0,
          "and never as a runway, which would invent a runway that is not there");
}

// The runway must be the WIDER of the two, not merely a different layer -- the
// widths are what carry the distinction, and swapping them draws a taxiway
// network that reads as a dozen runways.
void test_a_runway_is_wider_than_a_taxiway()
{
    const MapStyle_t style;
    check(map_widget::halfWidthFor(MapLayer::AerowayRunway, style) >
              map_widget::halfWidthFor(MapLayer::AerowayTaxiway, style),
          "the default runway half-width exceeds the taxiway one");
    check(map_widget::halfWidthFor(MapLayer::AerowaySurface, style) == 0.0f,
          "the surface is a fill and so carries no width");
}

// An apron is an AREA, and the surface layer is a fill -- so a polygon must
// reach it and a line must not, which is what the `fill` flag on the spec
// decides. Getting this backwards draws aprons as their outlines.
void test_an_apron_polygon_fills_and_does_not_line()
{
    const MapStyle_t style;
    const auto apron = tessellate(aerowayTile("apron", polygon({ square(0, 0, 2048) })), style);
    check(apron.layerIndexCount(MapLayer::AerowaySurface) == 6,
          "an apron polygon fills as two triangles");
    check(apron.layerVertexCount(MapLayer::AerowayTaxiway) == 0,
          "and does not also stroke into a line layer");
}

// Aeroway sits under water and over the ground it stands on. An airport beside
// a river is the case that decides it, and moving these rows re-orders the map.
void test_aeroway_draw_order_is_between_park_and_water()
{
    check(static_cast<int>(MapLayer::Park) < static_cast<int>(MapLayer::AerowaySurface),
          "the apron draws over the ground");
    check(static_cast<int>(MapLayer::AerowaySurface) < static_cast<int>(MapLayer::Water),
          "and under water");
    check(static_cast<int>(MapLayer::AerowaySurface) < static_cast<int>(MapLayer::AerowayRunway),
          "a runway draws over the apron it sits on");
    check(static_cast<int>(MapLayer::AerowayRunway) < static_cast<int>(MapLayer::RoadMinor),
          "and under the road network");
}


// ============================================================================
// Boundaries
// ============================================================================

// A national border is a heavier line than a city limit. Without reading
// admin_level every border on the map is one weight, which is what it was.
void test_a_country_border_outweighs_a_city_limit()
{
    const MapStyle_t style;
    const float country = halfPxOf(tessellate(boundaryTile(2), style), MapLayer::Boundary);
    const float state   = halfPxOf(tessellate(boundaryTile(4), style), MapLayer::Boundary);
    const float county  = halfPxOf(tessellate(boundaryTile(6), style), MapLayer::Boundary);
    const float city    = halfPxOf(tessellate(boundaryTile(8), style), MapLayer::Boundary);

    check(country > state, "a country border outweighs a state one");
    check(state > county, "a state border outweighs a county one");
    check(county > city, "a county border outweighs a city limit");

    // Shallow on purpose: past about a third the thinner line stops being
    // visible against landcover at all, which is worse than undifferentiated.
    check(city > country * 0.3f, "and the thinnest is still a visible line");
}

// `widths.boundary` stays the one knob for the whole set: it scales every
// level, and zero still hides all of them rather than only the country ones.
void test_the_boundary_width_still_scales_the_whole_ladder()
{
    MapStyle_t style;
    const float wide = halfPxOf(tessellate(boundaryTile(6), style), MapLayer::Boundary);

    style.widths.boundary *= 2.0;
    const float wider = halfPxOf(tessellate(boundaryTile(6), style), MapLayer::Boundary);
    check(std::abs(wider - (wide * 2.0f)) < 1e-4f, "doubling the width doubles every level");

    style.widths.boundary = 0.0;
    const auto hidden = tessellate(boundaryTile(6), style);
    check(hidden.layerVertexCount(MapLayer::Boundary) == 0,
          "a width of zero hides a county border, not just a country one");
}

// An archive built before admin_level was written must keep the single weight
// it had, rather than dropping every border to the thinnest rung.
void test_a_boundary_without_admin_level_keeps_full_weight()
{
    const MapStyle_t style;
    const float unlabelled =
        halfPxOf(tessellate(boundaryTile(0, /*writeLevel=*/false), style), MapLayer::Boundary);
    const float country = halfPxOf(tessellate(boundaryTile(2), style), MapLayer::Boundary);
    check(std::abs(unlabelled - country) < 1e-6f,
          "no admin_level draws at full weight rather than as a city limit");
}

// What indexing actually bought, measured rather than asserted in a comment.
//
// The comparison is exact rather than approximate: under the old scheme every
// index that exists today was a WRITTEN VERTEX, because each triangle wrote its
// three corners out in full. So the old size is `indices * sizeof(MapVertex)`
// and the new one is `vertices * sizeof(MapVertex) + indices * sizeof(index)`.
//
// Measured on the mix below: 28 752 bytes against 64 368, i.e. 2.24x.
//
// This matters beyond the upload: TileCache is bounded by BYTES, so geometry at
// under half the size is more than twice the tiles held for the same budget --
// which is what keeps a pan back over ground already visited from re-fetching.
void test_indexing_shrinks_a_tile_worth_of_geometry()
{
    const MapStyle_t style;

    // A tile-ish mix: a long winding road and a many-sided lake, which are the
    // two shapes the archive is mostly made of.
    std::vector<mvt::Point> road;
    for (int i = 0; i < 200; ++i)
    {
        road.push_back({ i * 20, 2048 + ((i % 2) * 40) });
    }

    std::vector<mvt::Point> lake;
    for (int i = 0; i < 200; ++i)
    {
        const double angle = (2.0 * 3.14159265358979323846 * i) / 200.0;
        lake.push_back({ 2048 + int(800.0 * std::cos(angle)), 2048 + int(800.0 * std::sin(angle)) });
    }

    mvt::Layer roads;
    roads.name = "transportation";
    roads.extent = 4096;
    roads.keys = { "class" };
    roads.values = { mvt::Value(std::in_place_type<std::string>, "primary") };
    mvt::Feature roadFeature = line(road);
    roadFeature.tags = { 0, 0 };
    roads.features.push_back(std::move(roadFeature));

    mvt::Layer water;
    water.name = "water";
    water.extent = 4096;
    water.features.push_back(polygon({ lake }));

    mvt::Tile tile;
    tile.layers.push_back(std::move(roads));
    tile.layers.push_back(std::move(water));

    const auto geometry = tessellate(tile, style);
    check(!geometry.indices.empty(), "the mixed tile produces geometry");

    const std::size_t indexed =
        (geometry.vertices.size() * sizeof(MapVertex)) +
        (geometry.indices.size() * sizeof(std::uint32_t));
    const std::size_t expanded = geometry.indices.size() * sizeof(MapVertex);

    check(indexed * 2 < expanded,
          "indexing more than halves the geometry: " + std::to_string(indexed) + " bytes against " +
              std::to_string(expanded) + " expanded");
}

// ============================================================================
// Grade separation
// ============================================================================

// The defect this exists to fix: a minor road crossing a motorway on a bridge
// was drawn FIRST, in RoadMinor, and the motorway painted straight over it --
// so the overpass read as the road underneath. Nothing in the tile said which
// was on top, and nothing in the renderer could have used it if it had.
void test_a_bridge_draws_above_every_road_at_grade()
{
    check(static_cast<int>(MapLayer::Rail) < static_cast<int>(MapLayer::RoadBridgeCasing),
          "a bridge draws after the last road at grade");
    check(static_cast<int>(MapLayer::RoadBridgeCasing) < static_cast<int>(MapLayer::RoadBridge),
          "and its casing draws under its own deck");
    check(static_cast<int>(MapLayer::Motorway) < static_cast<int>(MapLayer::RoadBridge),
          "so a minor road on a bridge lands over a motorway at grade");
}

// A road is in exactly ONE of the two sets. Leaving bridges in the surface
// layers as well would put a copy of every bridge back under the traffic it
// crosses -- the precise artefact the bridge layers exist to remove.
void test_a_bridge_leaves_the_surface_layers()
{
    const MapStyle_t style;

    const auto bridge =
        tessellate(roadTileBrunnel("minor", "bridge", { { 0, 2048 }, { 4096, 2048 } }), style);
    check(bridge.layerIndexCount(MapLayer::RoadMinor) == 0,
          "a minor road on a bridge is not also drawn at grade");
    check(bridge.layerIndexCount(MapLayer::RoadBridge) > 0, "it is drawn on the bridge layer");
    check(bridge.layerIndexCount(MapLayer::RoadBridgeCasing) > 0, "with a casing under it");

    const auto atGrade =
        tessellate(roadTileBrunnel("minor", "", { { 0, 2048 }, { 4096, 2048 } }), style);
    check(atGrade.layerIndexCount(MapLayer::RoadMinor) > 0, "a road at grade stays at grade");
    check(atGrade.layerIndexCount(MapLayer::RoadBridge) == 0, "and is not drawn as a bridge");
}

// A tunnel is not a bridge. Both carry `brunnel`, and treating the key as a
// boolean would draw every underpass on top of the road above it -- exactly
// backwards, and worse than the flat drawing it replaced.
void test_a_tunnel_is_not_drawn_as_a_bridge()
{
    const MapStyle_t style;
    const auto tunnel =
        tessellate(roadTileBrunnel("minor", "tunnel", { { 0, 2048 }, { 4096, 2048 } }), style);

    check(tunnel.layerIndexCount(MapLayer::RoadBridge) == 0, "a tunnel is not on the bridge layer");
    check(tunnel.layerIndexCount(MapLayer::RoadMinor) > 0,
          "and still draws, at grade, rather than vanishing");
}

// The bridge layers carry roads of EVERY class, so they must take each road's
// own width rather than one width for all of them -- otherwise a motorway
// bridge and a driveway bridge are the same line.
void test_a_bridge_keeps_its_road_class_width_and_colour()
{
    const MapStyle_t style;

    const auto motorway =
        tessellate(roadTileBrunnel("motorway", "bridge", { { 0, 2048 }, { 4096, 2048 } }), style);
    const auto minor =
        tessellate(roadTileBrunnel("minor", "bridge", { { 0, 2048 }, { 4096, 2048 } }), style);

    check(halfPxOf(motorway, MapLayer::RoadBridge) > halfPxOf(minor, MapLayer::RoadBridge),
          "a motorway bridge is wider than a minor road bridge");

    // And the deck keeps the road's colour, so a motorway bridge still reads as
    // a motorway. The casing is the only part with a colour of its own.
    const auto& deck = motorway.vertices[motorway.layerStart[std::size_t(MapLayer::RoadBridge)]];
    const auto plain = tessellate(roadTileBrunnel("motorway", "", { { 0, 2048 }, { 4096, 2048 } }),
                                  style);
    const auto& road = plain.vertices[plain.layerStart[std::size_t(MapLayer::Motorway)]];
    check(std::abs(deck.r - road.r) < 1e-6f && std::abs(deck.g - road.g) < 1e-6f &&
              std::abs(deck.b - road.b) < 1e-6f,
          "a bridge deck is the same colour as the road it carries");
}

// The casing has to be WIDER than the deck or it never shows, and it must
// vanish with the road rather than outlining nothing.
void test_the_bridge_casing_is_wider_than_its_deck()
{
    MapStyle_t style;
    const auto tile = roadTileBrunnel("primary", "bridge", { { 0, 2048 }, { 4096, 2048 } });

    const auto geometry = tessellate(tile, style);
    check(halfPxOf(geometry, MapLayer::RoadBridgeCasing) >
              halfPxOf(geometry, MapLayer::RoadBridge),
          "the casing is wider than the deck, so it shows either side of it");

    // A road switched off by a zero width stays off on its bridge, rather than
    // leaving a casing around nothing.
    style.widths.road_primary = 0.0;
    const auto hidden = tessellate(tile, style);
    check(hidden.layerIndexCount(MapLayer::RoadBridge) == 0, "a zero-width road has no deck");
    check(hidden.layerIndexCount(MapLayer::RoadBridgeCasing) == 0, "and no casing either");
}

// An archive built before `brunnel` was written must keep drawing every road at
// grade, rather than losing the whole network to an empty bridge layer.
void test_an_archive_without_brunnel_draws_everything_at_grade()
{
    const MapStyle_t style;
    const auto geometry = tessellate(roadTile("primary", { { 0, 2048 }, { 4096, 2048 } }), style);
    check(geometry.layerIndexCount(MapLayer::RoadPrimary) > 0,
          "with no brunnel key at all, the road still draws at grade");
    check(geometry.layerIndexCount(MapLayer::RoadBridge) == 0, "and nothing lands on the bridge layer");
}

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_road_classes_reach_the_right_layers();
    test_a_motorway_lands_in_both_casing_and_fill();
    test_layers_are_contiguous_and_ordered();

    test_a_runway_and_a_taxiway_land_in_different_layers();
    test_a_runway_is_wider_than_a_taxiway();
    test_an_apron_polygon_fills_and_does_not_line();
    test_aeroway_draw_order_is_between_park_and_water();

    test_a_country_border_outweighs_a_city_limit();
    test_the_boundary_width_still_scales_the_whole_ladder();
    test_a_boundary_without_admin_level_keeps_full_weight();

    test_a_bridge_draws_above_every_road_at_grade();
    test_a_bridge_leaves_the_surface_layers();
    test_a_tunnel_is_not_drawn_as_a_bridge();
    test_a_bridge_keeps_its_road_class_width_and_colour();
    test_the_bridge_casing_is_wider_than_its_deck();
    test_an_archive_without_brunnel_draws_everything_at_grade();

    test_a_square_becomes_two_triangles();
    test_a_hole_is_cut_rather_than_filled();
    test_two_polygons_in_one_feature_stay_separate();
    test_degenerate_rings_are_dropped();

    test_a_two_point_line_is_one_quad();
    test_indexing_shrinks_a_tile_worth_of_geometry();
    test_a_corner_shares_one_normal_so_there_is_no_notch();
    test_a_hairpin_bevels_rather_than_growing_a_spike();
    test_repeated_points_do_not_produce_nan();

    test_coordinates_are_tile_local_and_extent_normalised();
    test_every_geometry_gets_its_own_serial();
    test_style_switches_are_honoured();
    test_per_layer_widths_come_from_the_style();
    test_road_width_scale_is_applied_exactly_once();
    test_detail_thresholds_come_from_the_style();
    test_width_scale_tapers_with_zoom();
    test_an_empty_tile_yields_nothing_but_valid_offsets();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all tessellator checks passed");
    return 0;
}
