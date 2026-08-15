// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stitching a multipolygon relation's member ways into closed rings.
//
// This is the piece that turns a lake into a lake. OSM stores a large water
// body as a `type=multipolygon` relation whose members are open ways -- each one
// an arc of the shoreline, in no particular order and in no particular
// direction -- plus more ways for the islands. Nothing in the data says where a
// ring starts or which way round it goes; the only thing that ties two arcs
// together is that they share an end NODE.
//
// So this is graph work, not geometry work, and it is separated out for exactly
// that reason: it can be tested on a handful of arcs given in a deliberately
// awkward order, without a PBF anywhere near it.
//
// THE FAILURE THAT MATTERS is a ring that does not close. Handing a renderer an
// open ring makes it close the shape across whatever chord happens to be left,
// which paints water over a city. So an unclosed ring is DROPPED and counted --
// never closed by force.
#ifndef MAP_BUILD_RINGS_H
#define MAP_BUILD_RINGS_H

#include <cstdint>
#include <vector>

#include "osm/entity.h"

namespace map_build
{

// One member way of a relation, already resolved to coordinates.
//
// Only the END nodes are kept, because stitching only ever asks "do these two
// arcs meet?" -- carrying every node id of every member would multiply the
// memory this holds for nothing.
struct RingArc
{
    std::int64_t firstNode { 0 };
    std::int64_t lastNode { 0 };
    // Interleaved lat/lon, in the way's own direction.
    std::vector<osm::Coord> geometry;
    bool inner { false };
};

struct AssembledRings
{
    // Closed rings, each interleaved lat/lon, first point NOT repeated at the
    // end -- the closing edge is implied, as it is everywhere else in this tree.
    std::vector<std::vector<osm::Coord>> outer;
    std::vector<std::vector<osm::Coord>> inner;
    // Arcs that could not be joined into any closed ring.
    std::uint32_t abandonedArcs { 0 };
};

// Join arcs end-to-end into closed rings, reversing them where needed.
AssembledRings assembleRings(std::vector<RingArc>& arcs);

} // namespace map_build

#endif // MAP_BUILD_RINGS_H
