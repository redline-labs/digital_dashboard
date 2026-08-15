// SPDX-License-Identifier: GPL-3.0-or-later
//
// Building the contraction hierarchy. Workstation only -- this allocates
// gigabytes and runs for minutes; the vehicle only ever reads the result.
//
// The expanded graph is built here rather than stored, for the reason the graph
// format already gives: materialising it would roughly triple an 888 MB
// artifact to hold what a binary search derives for free. It is needed in memory
// during contraction and nowhere else.
//
// THE TRANSITION RULE IS COPIED FROM THE A* SEARCH ON PURPOSE. A hierarchy that
// permits a turn the plain router forbids does not fail loudly; it returns a
// faster route that a driver cannot follow. The two rules are stated once here
// and once in search.cpp, and `road_graph_test_contraction` asserts they agree
// by routing the same pairs both ways and comparing the costs.
#ifndef ROAD_GRAPH_CONTRACTION_H
#define ROAD_GRAPH_CONTRACTION_H

#include <cstdint>
#include <filesystem>

#include "road_graph/error.h"
#include "road_graph/graph.h"

namespace road_graph
{

struct ContractionOptions
{
    // How far a witness search may look before giving up and adding a shortcut
    // anyway. Bounded because the witness search is the whole cost of building:
    // an unbounded one is exact and takes days. Being too cautious only adds
    // shortcuts that were not needed -- it never makes a route wrong.
    std::size_t witnessSettleLimit { 200 };
    std::size_t witnessHopLimit { 5 };

    // Where to STOP contracting, as a fraction of the nodes.
    //
    // The last per cent of a road network is a dense core -- the motorway
    // junctions everything routes through -- and contracting it is where a
    // hierarchy build goes from minutes to hours: each of those nodes has a
    // large in-degree times out-degree, and every witness search runs inside the
    // same dense subgraph. Measured on the SoCal graph, 92% of the nodes
    // contract in about four minutes and the remaining 8% had not finished in
    // twenty.
    //
    // So the core is left uncontracted, all of it at one rank, and the query
    // searches it directly. That is a plain bidirectional Dijkstra over a few
    // hundred thousand nodes instead of nine million -- still a large win, and
    // exactly correct: `road_graph_test_contraction` runs the whole comparison
    // again with a deliberately large core to prove the two agree.
    //
    // 0.95 IS A MEASURED NUMBER, not a guess. On the SoCal graph it contracts in
    // seven minutes and leaves 475 k nodes in the core. Pushing to 0.98 with
    // looser witness limits was still running twenty minutes later, having spent
    // the extra time on the densest few per cent -- and the queries it would
    // have produced are not the ones that are slow. Raise it only with a
    // `map_build route` run to show it paid.
    double stopAtFraction { 0.95 };

    // Progress line every N contracted nodes. Zero is silent.
    std::uint64_t progressEvery { 250'000 };
};

struct ContractionStats
{
    std::uint64_t expandedNodes { 0 };
    // Nodes left uncontracted at the top of the hierarchy.
    std::uint64_t coreNodes { 0 };
    std::uint64_t originalArcs { 0 };
    std::uint64_t shortcuts { 0 };
    std::uint64_t witnessSearches { 0 };
    double buildSeconds { 0.0 };
    std::uint64_t bytes { 0 };
};

// Contract `graph` and write the overlay beside it.
Result<ContractionStats> buildOverlay(const Graph& graph, const std::filesystem::path& out,
                                      const ContractionOptions& options = {});

} // namespace road_graph

#endif // ROAD_GRAPH_CONTRACTION_H
