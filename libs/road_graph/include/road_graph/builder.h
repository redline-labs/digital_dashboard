// SPDX-License-Identifier: GPL-3.0-or-later
//
// Building a road graph.
//
// The caller hands over segments -- pieces of way between junctions, with
// geometry already resolved -- and this arranges them into the file layout
// format.h describes. Everything that makes the artifact fast to query happens
// here, once, on a workstation:
//
//   - nodes and segments are reordered onto a Hilbert curve, so a query touches
//     few pages (decision 5);
//   - the CSR is built after that reordering, so adjacency follows locality;
//   - the id and way indices are sorted, so a lookup is a binary search;
//   - a packed R-tree is built bottom-up over the already-ordered segments,
//     which is why it costs one pass and no comparisons.
//
// Held entirely in memory during the build. For a continental extract that is
// the largest thing the build touches after the node store, and it is why the
// build is workstation-only.
#ifndef ROAD_GRAPH_BUILDER_H
#define ROAD_GRAPH_BUILDER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "map_rules/classification.h"
#include "road_graph/error.h"
#include "road_graph/format.h"
#include "road_graph/geometry.h"

namespace road_graph
{

class Builder
{
  public:
    // One piece of way, between two junctions.
    struct SegmentInput
    {
        SegmentId id { 0 };
        std::int64_t osmWayId { 0 };

        // Interleaved lat/lon, at least two points, in the way's own direction.
        std::vector<Coord> geometry;

        // OSM node ids of the endpoints. Used to join segments into a graph;
        // the node INDEX is assigned here.
        std::int64_t fromNodeId { 0 };
        std::int64_t toNodeId { 0 };

        map_rules::RoadClassification classification;
        std::string name;
        std::string ref;
    };

    void add(SegmentInput&& segment);

    // A turn restriction, still in OSM terms.
    //
    // Taken as WAY ids rather than segment ids because that is what the
    // relation says, and resolving it needs the segment layout -- which does
    // not exist until write(). Resolution walks the way index to find the piece
    // of each way that touches the via node; without that index (decision 3)
    // this would be unanswerable.
    struct RestrictionInput
    {
        std::int64_t fromWayId { 0 };
        std::int64_t viaNodeId { 0 };
        std::int64_t toWayId { 0 };
        bool only { false };
    };

    void addRestriction(const RestrictionInput& restriction);

    struct RestrictionCounts
    {
        std::uint64_t offered { 0 };
        std::uint64_t resolved { 0 };
        // A way that contributed no routable segment, or a via node that is not
        // a junction. Counted rather than logged: on a continental extract
        // these run to thousands and each one is uninteresting on its own.
        std::uint64_t unresolved { 0 };
    };

    const RestrictionCounts& restrictionCounts() const { return mRestrictionCounts; }

    std::size_t segmentCount() const { return mSegments.size(); }

    // Sort, index and write. `builtAtUnixS` is passed in rather than read from
    // the clock so a build is reproducible and so the value can be pinned in a
    // test.
    Result<void> write(const std::filesystem::path& path, std::int64_t builtAtUnixS);

  private:
    struct Pending
    {
        SegmentInput input;
        NodeIndex from { kNoNode };
        NodeIndex to { kNoNode };
        std::uint32_t lengthCm { 0 };
    };

    NodeIndex nodeFor(std::int64_t osmNodeId, Coord lat, Coord lon);
    std::uint32_t internString(const std::string& text);

    std::vector<Pending> mSegments;
    std::unordered_map<std::int64_t, NodeIndex> mNodeIds;
    std::vector<NodeRecord> mNodes;

    std::string mStrings { std::string(1, '\0') };
    std::unordered_map<std::string, std::uint32_t> mStringOffsets;

    std::vector<RestrictionInput> mRestrictions;
    RestrictionCounts mRestrictionCounts;
};

} // namespace road_graph

#endif // ROAD_GRAPH_BUILDER_H
