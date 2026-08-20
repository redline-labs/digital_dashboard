// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning drawn ways into a vector tile pyramid.
//
// The layer names and the `class` values are NOT arbitrary: they are what
// dashboard/widgets/map/tessellator.cpp already switches on, so tiles built
// here render in the widget that exists rather than needing a new one. That is
// the whole cutover story -- see kLayerFor below.
//
// Three pieces of geometry work, each with a failure that renders:
//
//   SIMPLIFICATION drops points that do not change the shape at this zoom.
//   Douglas-Peucker with a tolerance of about half a tile unit: finer and the
//   tiles are needlessly large, coarser and roads visibly wander off their
//   junctions.
//
//   CLIPPING cuts a feature to the tile plus a buffer. WITHOUT the buffer,
//   every line ends exactly at the tile edge and a renderer's line joins leave
//   a seam down every boundary; with it, neighbouring tiles overlap and the
//   joins are hidden.
//
//   BUCKETING decides which tiles a feature touches. A feature's bounding box
//   is the cheap answer and is what is used; the alternative -- walking the
//   line and emitting only the tiles it really crosses -- saves work on long
//   diagonal features and costs more than it saves on everything else.
#ifndef MAP_BUILD_TILER_H
#define MAP_BUILD_TILER_H

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "mbtiles/writer.h"
#include "map_build/extract.h"

namespace map_build
{

struct TileOptions
{
    std::uint8_t minZoom { 0 };
    std::uint8_t maxZoom { 14 };

    // Tile-local coordinate space. 4096 is what everything in this tree assumes
    // by default, and the widget reads the per-layer value anyway.
    std::uint32_t extent { 4096 };

    // How far outside the tile to keep geometry, in extent units. 64/4096 is
    // 1/64th of a tile, which is comfortably wider than the widest road the
    // style draws.
    std::int32_t buffer { 64 };

    // Smallest bounding box a feature may have and still be written, in extent
    // units. Below this it is a dot, and carrying it costs bytes at every zoom
    // for something nobody can see.
    //
    // This is GENERALIZATION, and it is the reason low-zoom tiles are a usable
    // size. Zero disables it, which is the right setting for a debugging build
    // and the wrong one for a shipped archive.
    double minExtent { 2.0 };

    // Zoom at and above which lines keep their own identity, one feature per
    // source way. Below it, features sharing every attribute are merged.
    //
    // 13 is deliberate and is a CONTRACT, not a preference: a client joining a
    // tile feature back to the road graph does so at the zooms it drives at,
    // and merging makes that join lossy. Raising this trades tile size for
    // identity; lowering it trades identity for size.
    std::uint8_t mergeBelowZoom { 13 };

    // Progress line every N tiles. Zero is silent.
    std::uint32_t progressEvery { 5000 };
};

struct TileStats
{
    std::uint64_t features { 0 };
    // Line features folded into another by mergeBelowZoom.
    std::uint64_t mergedLines { 0 };
    std::uint64_t tiles { 0 };
    std::uint64_t bytes { 0 };
    std::uint64_t emptyTiles { 0 };
    // Features dropped because they became degenerate at this zoom -- a lake
    // smaller than a pixel, a road shorter than a tile unit. Expected to be
    // large at low zoom and near zero at z14.
    std::uint64_t droppedTooSmall { 0 };

    std::map<std::uint8_t, std::uint64_t> tilesPerZoom;
};

class Tiler
{
  public:
    void add(DrawInput&& feature);

    std::size_t featureCount() const { return mFeatures.size(); }

    // Build the pyramid and write it. `bounds` is written into the metadata,
    // which is what a client reads to know where the coverage is.
    mbtiles::Result<TileStats> write(mbtiles::Writer& writer, const TileOptions& options,
                                     const std::string& name, std::int32_t west,
                                     std::int32_t south, std::int32_t east, std::int32_t north);

  private:
    struct Prepared
    {
        // Web Mercator, normalised to [0,1] so a zoom is one multiply. The
        // projection appears exactly once per coordinate, which is what stops a
        // map ending up subtly offset.
        // Ring 0 is the line, the point, or the area's first outer ring;
        // anything after it is another ring of the same area. A multipolygon
        // arrives here as several, and everything else as exactly one, so the
        // two share one code path rather than a branch at every step.
        std::vector<std::vector<double>> worldXY;
        // Which of those rings are holes, parallel to worldXY. Roles are
        // carried rather than winding because winding is set later, after
        // projection -- Web Mercator flips y, so the sign changes there.
        std::vector<std::uint8_t> ringIsInner;
        map_rules::RenderClass renderClass { map_rules::RenderClass::None };
        // The `class` attribute for anything that is not a road, borrowed from
        // map_rules' static storage rather than copied -- see the field of the
        // same name on RoadClassification. Roads leave this empty and get their
        // class from roadClassFor(renderClass) instead.
        const char* className { "" };
        std::uint8_t minZoom { 255 };
        bool isArea { false };
        std::int64_t osmWayId { 0 };
        std::string name;
        std::string ref;
        std::uint16_t postedKph { 0 };
        bool hasPosted { false };
        std::uint8_t adminLevel { 0 };
        // Grade separation. `osmLayer` is OSM's own `layer` key and is NOT the
        // `layer` field below -- that one is the name of the tile layer this
        // feature is written into, and the collision is why this one is
        // spelled out.
        //
        // Without these there is no way to draw an overpass: a freeway and the
        // surface street beneath it are the same two crossing lines.
        bool isBridge { false };
        bool isTunnel { false };
        std::int8_t osmLayer { 0 };
        std::uint8_t laneCount { 0 };
        bool onewayForward { false };
        bool onewayBackward { false };
        // Set only by the label layers; empty means layerFor(renderClass).
        const char* layer { "" };
        std::vector<std::pair<std::string, std::string>> attributes;

        // A label rather than a line. Neither simplified nor clipped: a point
        // has no shape to simplify, and a clipper that ran on it would be
        // deciding whether a city exists rather than where its label sits.
        bool isPoint { false };
        map_rules::PlaceKind placeKind { map_rules::PlaceKind::None };
        std::uint8_t labelRank { 255 };
        std::uint32_t population { 0 };
    };

    // A DEQUE AND NOT A VECTOR, and the reason is peak memory rather than taste.
    //
    // Prepared is about 200 bytes and a regional extract holds eleven million of
    // them. A vector grows by doubling, so the last reallocation alone holds the
    // old array and the new one at once -- ten gigabytes of transient, on top of
    // everything the coordinates already cost, for a container that is only ever
    // appended to and then walked in order. A deque grows in blocks, never
    // copies what it already holds, and the extra indirection per element is
    // invisible against the geometry work done inside the loop.
    std::deque<Prepared> mFeatures;
};

// Which layer a render class belongs in, in the vocabulary the widget's
// tessellator already switches on. Empty means "not drawn".
const char* layerFor(map_rules::RenderClass value);

// The `class` attribute the tessellator reads to pick a road's priority.
const char* roadClassFor(map_rules::RenderClass value);

} // namespace map_build

#endif // MAP_BUILD_TILER_H
