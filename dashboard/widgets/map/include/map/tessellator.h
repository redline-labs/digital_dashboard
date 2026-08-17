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

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "mvt/tile.h"

#include "map/style.h"

namespace map_widget
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
    Water,
    Waterway,
    Building,
    RoadMinor,
    RoadMajor,
    RoadPrimary,
    MotorwayCasing,
    Motorway,
    Rail,
    Boundary,
    // Race tracks, ON TOP of everything else. A circuit is the thing being
    // looked at when it is on screen at all, and it comes from a separate
    // archive drawn as an overlay, so burying it under the basemap's roads
    // would defeat the point. The trade is real and deliberate: on a street
    // circuit the surface covers the public roads that ARE the circuit.
    TrackSurface,
    TrackCentre,
};

inline constexpr std::size_t kMapLayerCount = 15;

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

// Handed out by TileGeometry's constructor. Tiles are tessellated on zenoh
// threads, so this is atomic.
std::uint64_t nextSerial();

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
    std::array<std::uint32_t, kMapLayerCount + 1> layerStart {};

    std::uint32_t layerVertexCount(MapLayer layer) const
    {
        const auto i = static_cast<std::size_t>(layer);
        return layerStart[i + 1] - layerStart[i];
    }

    bool empty() const { return vertices.empty(); }
};

TileGeometry tessellate(const mvt::Tile& tile, const MapStyle_t& style);

// How much of the baked-in width to actually use at this zoom. Applied in the
// shader as a uniform, so zooming never invalidates a tessellation.
float widthScaleForZoom(double zoom);

} // namespace map_widget

#endif // MAP_TESSELLATOR_H
