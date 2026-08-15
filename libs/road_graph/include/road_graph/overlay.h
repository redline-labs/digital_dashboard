// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading a contraction hierarchy and routing with it.
//
// The read side is deliberately tiny: mmap, validate against the graph, and run
// a bidirectional Dijkstra that only ever walks UPWARD. That is the whole trick
// -- both searches climb the hierarchy, they meet somewhere near its top, and
// the number of nodes either one settles is a few hundred instead of a few
// hundred thousand.
//
// The query returns the same Route the plain router does, so a caller can switch
// between them without knowing which answered. `road_graph_test_contraction`
// exists to keep that promise honest: it routes the same pairs both ways and
// requires the COSTS to match, because a hierarchy that is subtly wrong returns
// a plausible route quickly rather than failing.
#ifndef ROAD_GRAPH_OVERLAY_H
#define ROAD_GRAPH_OVERLAY_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

#include "road_graph/error.h"
#include "road_graph/graph.h"
#include "road_graph/overlay_format.h"
#include "road_graph/search.h"

namespace road_graph
{

class Overlay
{
  public:
    // Opens and validates AGAINST THE GRAPH. An overlay built from a different
    // graph would not crash -- its shortcuts name edge indices that still exist
    // and now mean other roads -- so this refuses rather than routing.
    static Result<Overlay> open(const std::filesystem::path& path, const Graph& graph);

    Overlay(Overlay&&) noexcept;
    Overlay& operator=(Overlay&&) noexcept;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    ~Overlay();

    const OverlayHeader& header() const { return *mHeader; }

    // Edges that END at a road-graph node. The graph itself stores only
    // outgoing edges; this is what seeds the backward search.
    std::span<const std::uint32_t> incomingEdges(NodeIndex node) const;

  private:
    Overlay() = default;

    Result<void> bind(const Graph& graph);

    friend std::optional<Route> findRouteVia(const Graph&, const Overlay&, NodeIndex, NodeIndex);

    void* mMapping { nullptr };
    std::size_t mSize { 0 };

    const OverlayHeader* mHeader { nullptr };
    std::span<const std::uint32_t> mRanks;
    std::span<const std::uint32_t> mUpOffsets;
    std::span<const OverlayArc> mUpArcs;
    std::span<const std::uint32_t> mDownOffsets;
    std::span<const OverlayArc> mDownArcs;
    std::span<const std::uint32_t> mIncomingOffsets;
    std::span<const std::uint32_t> mIncomingEdges;
};

// The same answer findRoute() gives, by a different road.
std::optional<Route> findRouteVia(const Graph& graph, const Overlay& overlay, NodeIndex from,
                                  NodeIndex to);

} // namespace road_graph

#endif // ROAD_GRAPH_OVERLAY_H
