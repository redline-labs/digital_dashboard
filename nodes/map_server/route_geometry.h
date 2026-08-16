// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thinning a route's polyline before it goes on the wire.
//
// A route is always drawn at some zoom, and the graph's geometry is finer than
// any of them: at z12 a 40 km route's centimetre-accurate polyline is thousands
// of points inside a single screen pixel. The caller knows its zoom and the
// server knows the geometry, so the caller says how much precision it needs
// rather than being sent all of it.
//
// No zenoh and no capnp here, so it tests as arithmetic.
#ifndef MAP_SERVER_ROUTE_GEOMETRY_H
#define MAP_SERVER_ROUTE_GEOMETRY_H

#include <cstdint>
#include <span>
#include <vector>

#include "road_graph/geometry.h"

namespace map_server
{

struct SimplifiedRoute
{
    // Interleaved lat/lon, as the input is.
    std::vector<road_graph::Coord> geometry;
    // Where each segment's run begins, in COORDINATE PAIRS. One more entry
    // than there are segments.
    std::vector<std::uint32_t> segmentStarts;
};

// Douglas-Peucker, applied to each segment's run INDEPENDENTLY.
//
// Per segment rather than over the whole polyline, because the boundaries are
// what let a client say which points belong to which road -- simplifying
// across them would drop the join and leave segmentStarts pointing at the
// wrong places. It also means a segment never loses its first or last point,
// so consecutive runs still meet.
//
// A tolerance of zero (or negative) copies the input through.
SimplifiedRoute simplifyPerSegment(std::span<const road_graph::Coord> geometry,
                                   std::span<const std::uint32_t> segmentStarts,
                                   double toleranceM);

} // namespace map_server

#endif // MAP_SERVER_ROUTE_GEOMETRY_H
