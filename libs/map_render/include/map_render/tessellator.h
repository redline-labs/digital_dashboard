// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vector tiles into triangles.
//
// The CPU half of the GPU renderer, and deliberately the only half that knows
// what a road is. No Qt, no QRhi, no camera: a tile tessellates once, into
// TILE-LOCAL coordinates, and the result is reused for every frame at every
// zoom, rotation and position until the style changes. That is the whole reason
// the GPU path is fast -- not the drawing, but the not-redoing.
//
// Two things here are worth more than they look:
//
//   * Polygons are triangulated with holes (earcut). Fan triangulation is the
//     obvious shortcut and it fills every concave shape wrongly -- a lake over
//     its own island, a spike across a park.
//
//   * Polylines get MITRE JOINS. Emitting an independent quad per segment is
//     simpler and leaves a visible notch at every corner of every road, which
//     at z14 is thousands of them.
#ifndef MAP_TESSELLATOR_H
#define MAP_TESSELLATOR_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "mvt/tile.h"

#include "map_render/style.h"

namespace map_render
{

// What the vertex shader consumes. Kept to 36 bytes and one layout so the whole
// map is a single pipeline; anything that varies per layer is baked in here.
struct MapVertex
{
    float x { 0.f }, y { 0.f };      // tile-local, [0,1] across the tile
    float nx { 0.f }, ny { 0.f };    // unit perpendicular for line expansion
    float halfPx { 0.f };            // half width in screen px at full scale
    float r { 0.f }, g { 0.f }, b { 0.f }, a { 1.f };
};
static_assert(sizeof(MapVertex) == 9 * sizeof(float));

// The draw order, and therefore the map. There is no depth buffer and no
// sorting: moving a row here moves what covers what.
//
// Drawn layer-major ACROSS tiles -- every tile's water, then every tile's
// roads. Drawing tile-by-tile instead would let one tile's landcover bury its
// neighbour's motorway, which looks like missing data along tile seams.
enum class MapLayer : std::uint8_t
{
    Landcover,
    Landuse,
    Park,
    // Aprons, aerodrome grounds and helipads: ground surface, so it sits with
    // the other ground surfaces and UNDER water. An airport beside a river is
    // the case that decides the order, and the river wins.
    AerowaySurface,
    Water,
    Waterway,
    Building,
    // Runways and taxiways, drawn OVER the surface they sit on and under the
    // road network -- the two do not overlap in practice, so the order between
    // them is about which reads as on top at an airport's edge, and a road
    // bridge over a taxiway is the commoner picture.
    //
    // Two layers rather than one because a runway is tens of metres of concrete
    // and a taxiway is a lane: drawn at one width, either the runway vanishes
    // or the taxiways smear into it.
    AerowayTaxiway,
    AerowayRunway,
    RoadMinor,
    RoadMajor,
    RoadPrimary,
    MotorwayCasing,
    Motorway,
    Rail,
    // Grade separation. A bridge is drawn AFTER every road at grade, which is
    // the whole point: without it a minor road crossing a motorway on a bridge
    // is drawn first and the motorway paints straight over it, so the overpass
    // reads as the road underneath.
    //
    // Two layers rather than one because a bridge needs a casing to separate it
    // from whatever it crosses -- a bridge deck the same colour as the road
    // below, meeting it at a crossing, says nothing about which is on top.
    //
    // These carry roads of EVERY class, with per-feature colour and width taken
    // from the class's own layer, rather than a bridge variant of each of the
    // six road layers.
    RoadBridgeCasing,
    RoadBridge,
    Boundary,
    // Race tracks, ON TOP of everything else. A circuit is the thing being
    // looked at when it is on screen at all, and it comes from a separate
    // archive drawn as an overlay, so burying it under the basemap's roads
    // would defeat the point. The trade is real and deliberate: on a street
    // circuit the surface covers the public roads that ARE the circuit.
    TrackSurface,
    TrackCentre,
};

inline constexpr std::size_t kMapLayerCount = 20;

// Nothing else checks that the enum and kMapLayerCount agree, and a mismatch
// is not a compile error in the obvious place: TileGeometry::layerStart is
// sized from the count, so an enumerant past the end is an out-of-bounds write
// that still draws SOMETHING.
static_assert(static_cast<std::size_t>(MapLayer::TrackCentre) + 1 == kMapLayerCount,
              "kMapLayerCount must match the last MapLayer enumerator");

const char* to_string(MapLayer layer);

// Below this camera zoom a layer is not drawn at all. Buildings at z8 are
// several thousand polygons per tile, none of them a pixel across.
//
// The user's threshold, from MapStyle_t::detail. The archive's own per-layer
// minzoom still applies underneath it, so the effective floor is whichever is
// higher -- see docs/map.md.
double layerMinZoom(MapLayer layer, const MapStyle_t& style);

// Half-width in screen pixels at full scale, from MapStyle_t::widths. Zero for
// fills, and zero for any line the user has switched off by setting its width
// to zero. NOT multiplied by road_width_scale: that rides the shader uniform.
float halfWidthFor(MapLayer layer, const MapStyle_t& style);

// Which OpenMapTiles road class belongs in which layer. An unrecognised class
// draws as a minor road rather than vanishing, because a hole in the road
// network is worse than a road of the wrong width.
int roadPriority(std::string_view className);

// The layer a road of this priority is drawn in at grade.
//
// Exists so the bridge layers can borrow a road's own colour and width instead
// of restating the ladder: a bridge is the same road, drawn higher up.
MapLayer roadLayerFor(int priority);

// Handed out by TileGeometry's constructor. Tiles are tessellated on zenoh
// threads, so this is atomic.
std::uint64_t nextSerial();

// Where one road feature's triangles live, so a client can find them again.
//
// This is the join `map_build` stamps an OSM way id on every feature FOR: the
// route a driver is following, and the road they are on, come back from
// `map/nearest` and `map/route` as way ids, and highlighting them means
// recolouring geometry that is already on the GPU rather than overlaying a
// second polyline that can diverge from it (docs/map_build.md).
//
// Roads only. Recording a range for every landcover polygon would double the
// size of this for something nothing joins on.
struct FeatureRange
{
    std::uint64_t osmWayId { 0 };
    std::uint32_t indexStart { 0 };
    std::uint32_t indexCount { 0 };
};

// One tile's geometry, ready to upload.
//
// Vertices for layer i occupy [layerStart[i], layerStart[i+1]). One buffer per
// tile rather than one per layer, because the draw loop only needs an offset
// and a count to pick a layer out of it.
struct TileGeometry
{
    // Unique for the life of the process, assigned on construction.
    //
    // It exists so the renderer can tell "the same triangles as last frame"
    // from "different triangles for the same tile" without comparing the
    // vertices. The obvious shortcut -- comparing the shared_ptr's address --
    // is WRONG: geometry freed by a cache eviction is routinely handed straight
    // back by the allocator, so a replacement tile can land on the dead one's
    // address and the renderer keeps drawing what it uploaded before.
    std::uint64_t serial { nextSerial() };

    std::vector<MapVertex> vertices;
    // Triangles, as offsets into `vertices` -- TILE-LOCAL, so a tile's geometry
    // is still independent of where in the shared buffer it lands. The renderer
    // passes the tile's base vertex to drawIndexed() rather than rewriting
    // these.
    //
    // Indexed rather than expanded, because the expansion was most of the
    // memory: a polyline shares a vertex between the two triangles either side
    // of it and between the two segments either side of THAT, so writing them
    // out cost six vertices per segment where two per point will do. A dense
    // z14 tile was ~24k vertices at 36 bytes; this is where that goes.
    std::vector<std::uint32_t> indices;

    // Vertices for layer i occupy [layerStart[i], layerStart[i+1]), and the
    // indices that draw it [layerIndexStart[i], layerIndexStart[i+1]).
    std::array<std::uint32_t, kMapLayerCount + 1> layerStart {};
    std::array<std::uint32_t, kMapLayerCount + 1> layerIndexStart {};

    // Road features that carry an OSM way id, SORTED BY IT so a lookup is a
    // binary search. A tile holds a few thousand roads and the highlight set is
    // a handful of ids, so the search runs over the smaller side.
    std::vector<FeatureRange> roads;

    // Every range drawing `osmWayId`. A way clipped by the tiler arrives as
    // several features, and a road drawn with a casing occupies two layers, so
    // one id legitimately maps to more than one range.
    template <typename Fn>
    void forEachRoadRange(std::uint64_t osmWayId, Fn&& fn) const
    {
        auto at = std::lower_bound(roads.begin(), roads.end(), osmWayId,
                                   [](const FeatureRange& range, std::uint64_t id) {
                                       return range.osmWayId < id;
                                   });
        for (; at != roads.end() && at->osmWayId == osmWayId; ++at)
        {
            fn(*at);
        }
    }

    std::uint32_t layerVertexCount(MapLayer layer) const
    {
        const auto i = static_cast<std::size_t>(layer);
        return layerStart[i + 1] - layerStart[i];
    }

    // How many indices draw this layer, i.e. three per triangle. This is the
    // count a draw call wants; layerVertexCount() is how much room the layer
    // takes in the vertex buffer, and since indexing they are no longer the
    // same number.
    std::uint32_t layerIndexCount(MapLayer layer) const
    {
        const auto i = static_cast<std::size_t>(layer);
        return layerIndexStart[i + 1] - layerIndexStart[i];
    }

    bool empty() const { return indices.empty(); }
};

// Read a number out of an MVT attribute. The tiler writes rank, population
// and admin_level through `builder.number()`, which picks an integer or a
// double encoding by value, so both have to be accepted -- reading only one
// of them yields a silent zero. Shared between the tessellator and the label
// pass, which were once two byte-identical copies of it.
std::optional<double> attributeNumber(const mvt::Layer& layer, const mvt::Feature& feature,
                                      std::string_view key);

TileGeometry tessellate(const mvt::Tile& tile, const MapStyle_t& style);

// How much of the baked-in width to actually use at this zoom. Applied in the
// shader as a uniform, so zooming never invalidates a tessellation.
float widthScaleForZoom(double zoom);

} // namespace map_render

#endif // MAP_TESSELLATOR_H
