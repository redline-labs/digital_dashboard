// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading a road graph.
//
// The file is MMAP'D AND CONST AFTER OPEN. That is what lets nodes/map_server
// answer queries on several zenoh threads with no lock at all -- unlike
// mbtiles::Archive, which holds a mutex because SQLite prepared statements are
// not shareable -- and what lets nodes/map_match hold the same file at the same
// time without either process knowing about the other.
//
// Demand paging is the whole storage strategy. Nothing here reads the file into
// memory, so opening a continental graph costs a few page faults rather than
// twenty gigabytes, and the pages a route actually touches are the pages it
// pulls in. That only works because the build ordered everything by Hilbert
// curve; see decision 5 in format.h.
#ifndef ROAD_GRAPH_GRAPH_H
#define ROAD_GRAPH_GRAPH_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "road_graph/error.h"
#include "road_graph/format.h"
#include "road_graph/geometry.h"

namespace road_graph
{

// One candidate road for a position.
struct Match
{
    SegmentIndex segment { kNoSegment };
    // Perpendicular distance to the segment's geometry. NOT an error estimate:
    // OSM centrelines are routinely several metres off, and with an RTK fix the
    // receiver is the more accurate of the two.
    double distanceM { 0.0 };
    // Along the segment's own geometry, in its own direction, so it does not
    // change meaning on a U-turn.
    std::uint32_t offsetCm { 0 };
    // The segment's bearing at the match point, degrees from true north.
    double bearingDeg { 0.0 };
    // Where the match landed.
    Coord lat { 0 };
    Coord lon { 0 };
};

class Graph
{
  public:
    // Opens and validates. Does NOT create; a graph is built by tools/map_build.
    static Result<Graph> open(const std::filesystem::path& path);

    Graph(Graph&&) noexcept;
    Graph& operator=(Graph&&) noexcept;
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    ~Graph();

    const FileHeader& header() const { return *mHeader; }

    std::span<const NodeRecord> nodes() const { return mNodes; }
    std::span<const SegmentRecord> segments() const { return mSegments; }
    std::span<const EdgeRecord> edges() const { return mEdges; }

    // Directed edges leaving a node.
    std::span<const EdgeRecord> edgesFrom(NodeIndex node) const
    {
        if (node + 1 >= mCsrOffsets.size())
        {
            return {};
        }
        const std::uint32_t begin = mCsrOffsets[node];
        const std::uint32_t end = mCsrOffsets[node + 1];
        return mEdges.subspan(begin, end - begin);
    }

    // A segment's polyline, as interleaved lat/lon pairs, ALWAYS in the
    // segment's own direction. An edge running the other way reverses at the
    // point of use rather than owning a copy.
    std::span<const Coord> geometryOf(const SegmentRecord& segment) const
    {
        return mGeometry.subspan(segment.geometryOffset * 2, segment.geometryCount * 2);
    }

    std::string_view string(std::uint32_t offset) const;

    std::string_view nameOf(const SegmentRecord& segment) const
    {
        return string(segment.nameOffset);
    }
    std::string_view refOf(const SegmentRecord& segment) const { return string(segment.refOffset); }

    // Stable id -> array position. Binary search over the sorted index.
    //
    // This is the whole point of decision 2: everything off the wire arrives as
    // an id, and everything in here is an index, and the translation is here
    // rather than assumed.
    std::optional<SegmentIndex> indexOf(SegmentId id) const;

    // The segments a way was split into, in order along the way. Empty if the
    // way contributed nothing routable. Decision 3 -- stage 4 resolves turn
    // restrictions through this.
    std::span<const SegmentIndex> segmentsOfWay(std::int64_t wayId) const;

    // Whether a vehicle on `fromSegment` may continue onto `toSegment` at
    // `viaNode`.
    //
    // Consulted per transition during a search rather than baked into an
    // edge-expanded graph: expansion multiplies the edge count by average
    // degree, which would roughly triple the artifact to store something a
    // binary search derives for free. See TurnRestrictionRecord.
    bool turnAllowed(SegmentIndex fromSegment, NodeIndex viaNode, SegmentIndex toSegment) const;

    std::span<const TurnRestrictionRecord> turnRestrictions() const { return mRestrictions; }

    // Segments whose bounding box intersects the query box.
    void queryBox(Coord west, Coord south, Coord east, Coord north,
                  const std::function<void(SegmentIndex)>& visit) const;

    // Nearest routable segments, ordered by distance.
    //
    // `headingDeg`, when given, ranks candidates whose bearing disagrees lower.
    // That is what separates a divided highway from its own opposite
    // carriageway, and a road from the frontage road beside it -- the single
    // most common wrong answer a matcher gives.
    std::vector<Match> nearest(Coord lat, Coord lon, double radiusM,
                               std::size_t maxCandidates,
                               std::optional<double> headingDeg = std::nullopt) const;

  private:
    Graph() = default;

    Result<void> bind();
    std::span<const std::byte> section(Section kind) const;

    struct Mapping;
    std::unique_ptr<Mapping> mMapping;

    const FileHeader* mHeader { nullptr };
    std::span<const SectionEntry> mSections;

    std::span<const NodeRecord> mNodes;
    std::span<const SegmentRecord> mSegments;
    std::span<const EdgeRecord> mEdges;
    std::span<const Coord> mGeometry;
    std::span<const char> mStrings;
    std::span<const std::uint32_t> mCsrOffsets;
    std::span<const WayIndexEntry> mWayIndex;
    std::span<const SegmentIdIndexEntry> mIdIndex;
    std::span<const RTreeNode> mRTree;
    std::span<const TurnRestrictionRecord> mRestrictions;
    // Level offsets of the packed tree, root last.
    std::vector<std::uint32_t> mRTreeLevels;
};

} // namespace road_graph

#endif // ROAD_GRAPH_GRAPH_H
