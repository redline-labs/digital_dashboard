// SPDX-License-Identifier: GPL-3.0-or-later
//
// Searching the graph.
//
// Two things live here, and they are different sizes:
//
//   boundedDistance() -- how far apart two junctions are, giving up past a
//   limit. Used by the matcher's transition probability, dozens of times per
//   GNSS fix, so it must stay small and must NOT expand the whole map when the
//   answer is "not close".
//
//   findRoute() -- bidirectional A* between two points. Fine to a few hundred
//   kilometres; a continental query needs the preprocessing overlay that stage
//   6 adds, which will slot in beside this rather than replace it.
#ifndef ROAD_GRAPH_SEARCH_H
#define ROAD_GRAPH_SEARCH_H

#include <cstdint>
#include <optional>
#include <vector>

#include "road_graph/graph.h"

namespace road_graph
{

// Shortest driving distance between two junctions, in metres.
//
// Gives up and returns nothing once every frontier node is past `limitM`. That
// bound is the whole point: the matcher asks this about candidates that are
// usually metres apart, and an unbounded search would expand a city to discover
// that two roads do not connect.
std::optional<double> boundedDistance(const Graph& graph, NodeIndex from, NodeIndex to,
                                      double limitM);

struct Route
{
    // Segments traversed, in order.
    std::vector<SegmentIndex> segments;
    // Interleaved lat/lon, in travel order, with the reversal of any segment
    // traversed against its stored direction already applied.
    std::vector<Coord> geometry;
    double distanceM { 0.0 };
    double durationS { 0.0 };
};

// Bidirectional A* between two junctions.
//
// The heuristic is straight-line distance divided by the fastest speed in the
// graph, which is admissible -- it can never overestimate -- so the first
// meeting is optimal. Overestimating would be faster and would quietly return
// routes that are not the shortest, which nobody would notice until someone
// compared against a map.
std::optional<Route> findRoute(const Graph& graph, NodeIndex from, NodeIndex to,
                               std::size_t maxSettled = 2'000'000);

} // namespace road_graph

#endif // ROAD_GRAPH_SEARCH_H
