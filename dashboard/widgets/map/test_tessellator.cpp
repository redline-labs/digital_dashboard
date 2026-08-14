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

    // A quad is two triangles: 6 vertices, no index buffer.
    check(geometry.layerVertexCount(MapLayer::Water) == 6,
          "a 4-point ring triangulates to 2 triangles, got " +
              std::to_string(geometry.layerVertexCount(MapLayer::Water)));
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

    check(withHole.layerVertexCount(MapLayer::Water) > solid.layerVertexCount(MapLayer::Water),
          "a ring with a hole needs MORE triangles than one without -- the hole is cut around, "
          "not filled");

    // And no triangle may have its centroid inside the hole.
    const auto& v = withHole.vertices;
    int inside = 0;
    for (std::size_t i = 0; i + 2 < v.size(); i += 3)
    {
        const float cx = (v[i].x + v[i + 1].x + v[i + 2].x) / 3.0f;
        const float cy = (v[i].y + v[i + 1].y + v[i + 2].y) / 3.0f;
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
    check(geometry.layerVertexCount(MapLayer::RoadPrimary) == 6,
          "one segment is one quad, 6 vertices, got " +
              std::to_string(geometry.layerVertexCount(MapLayer::RoadPrimary)));
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
    check(count == 12, "two segments are two quads");
    if (count != 12)
    {
        return;
    }

    // Vertex 2 and 3 of the first quad are at the shared point; the second
    // quad's first two vertices are at the same point. Their normals must
    // match, or the quads do not line up.
    const MapVertex& endOfFirst = v[start + 2];
    const MapVertex& startOfSecond = v[start + 6];
    check(std::abs(endOfFirst.x - startOfSecond.x) < 1e-6f &&
              std::abs(endOfFirst.y - startOfSecond.y) < 1e-6f,
          "the two segments meet at the same point");
    check(std::abs(endOfFirst.nx - startOfSecond.nx) < 1e-5f &&
              std::abs(endOfFirst.ny - startOfSecond.ny) < 1e-5f,
          "and share one normal, so the quads meet exactly");

    // A 90-degree turn mitres to 1/cos(45) = sqrt(2) times unit length.
    const float mitreLength =
        std::sqrt((endOfFirst.nx * endOfFirst.nx) + (endOfFirst.ny * endOfFirst.ny));
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
    check(geometry.layerVertexCount(MapLayer::RoadPrimary) == 6,
          "and the duplicate points collapse to one segment");
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

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_road_classes_reach_the_right_layers();
    test_a_motorway_lands_in_both_casing_and_fill();
    test_layers_are_contiguous_and_ordered();

    test_a_square_becomes_two_triangles();
    test_a_hole_is_cut_rather_than_filled();
    test_two_polygons_in_one_feature_stay_separate();
    test_degenerate_rings_are_dropped();

    test_a_two_point_line_is_one_quad();
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
