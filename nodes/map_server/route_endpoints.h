// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning two snapped positions into the two junctions a search runs between.
//
// Separated from the queryable so it can be tested without a bus. Everything
// here is arithmetic on a snapped Match; nothing touches zenoh or capnp.
#ifndef MAP_SERVER_ROUTE_ENDPOINTS_H
#define MAP_SERVER_ROUTE_ENDPOINTS_H

#include <optional>

#include "road_graph/graph.h"

namespace map_server
{

struct RouteEndpoints
{
    // What the search runs between.
    road_graph::NodeIndex startNode { 0 };
    road_graph::NodeIndex endNode { 0 };

    // Which way the vehicle is travelling along its own segment. With no
    // heading there is nothing to know, so the segment's own direction stands
    // -- the same rule handleNearest uses.
    bool startForward { true };

    // The pieces the search does NOT cover: the part of the start segment
    // still ahead of the vehicle, and the part of the destination segment
    // before the destination. Adding them is what makes a reported distance
    // door to door rather than junction to junction.
    double startRemainingM { 0.0 };
    double endLeadInM { 0.0 };
};

RouteEndpoints resolveEndpoints(const road_graph::Graph& graph, const road_graph::Match& start,
                                const road_graph::Match& finish,
                                std::optional<double> fromHeadingDeg);

} // namespace map_server

#endif // MAP_SERVER_ROUTE_ENDPOINTS_H
