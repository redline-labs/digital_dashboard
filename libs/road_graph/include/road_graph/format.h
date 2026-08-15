// SPDX-License-Identifier: GPL-3.0-or-later
//
// The on-disk road graph: identities, records, sections.
//
// Six decisions here are the ones a later stage cannot undo, so each is stated
// where it is made rather than in a design document nobody reads:
//
//  1. TWO LEVELS OF IDENTITY. A *segment* is undirected and owns the name, the
//     class and the posted limit; a *directed edge* owns cost, access and
//     direction. The horizon publishes segments and the router walks edges.
//     Backwards, and the road-name display flickers on a U-turn -- silently,
//     because both are uint64.
//
//  2. THE PUBLISHED ID IS NOT AN ARRAY INDEX. Edge expansion renumbers,
//     partitioning reorders, Hilbert ordering reorders again, and a rebuild
//     happens on every OSM refresh. SegmentId is derived from the source way,
//     so it survives all of that; the index is an implementation detail that
//     never reaches the wire.
//
//  3. THE WAY INDEX STAYS IN THE ARTIFACT. Turn restrictions name from-way,
//     via and to-way, and once ways are split at junctions there is no way to
//     resolve from-way to the right incident segment without it. Dropping it
//     makes stage 4 a re-extract rather than a feature.
//
//  4. GEOMETRY IS STORED ONCE and referenced by (offset, count) with direction
//     on the edge. Edge expansion multiplies edges by average degree; inline
//     geometry would duplicate the whole coordinate array at that point.
//
//  5. NODES AND SEGMENTS ARE ORDERED BY HILBERT CURVE. OSM ids are
//     chronological and spatially random; an id-ordered continental graph
//     faults on most of an A* expansion. Free at build time, invisible on a
//     SoCal test archive -- which is exactly why it has to be designed in.
//
//  6. THE SECTION TABLE IS EXTENSIBLE. Stage 4's turn graph and stage 6's
//     per-profile overlays are additive sections keyed by the same stable ids,
//     so neither rewrites what is already here.
#ifndef ROAD_GRAPH_FORMAT_H
#define ROAD_GRAPH_FORMAT_H

#include <cstddef>
#include <cstdint>

namespace road_graph
{

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// Undirected, named, and STABLE ACROSS REBUILDS: derived from the OSM way and
// the position of this piece within it, not from where it landed in an array.
using SegmentId = std::uint64_t;

// A way is split into segments at every junction. 20 bits is a million pieces
// of one way, which no real way approaches; the remaining 44 bits hold a way id
// up to 1.7e13, against a current maximum around 1.4e9.
inline constexpr int kSegmentOrdinalBits = 20;
inline constexpr std::uint64_t kSegmentOrdinalMask = (std::uint64_t { 1 } << kSegmentOrdinalBits) - 1;

constexpr SegmentId makeSegmentId(std::int64_t wayId, std::uint32_t ordinal)
{
    return (static_cast<std::uint64_t>(wayId) << kSegmentOrdinalBits) |
           (static_cast<std::uint64_t>(ordinal) & kSegmentOrdinalMask);
}

constexpr std::int64_t wayOf(SegmentId id)
{
    return static_cast<std::int64_t>(id >> kSegmentOrdinalBits);
}

constexpr std::uint32_t ordinalOf(SegmentId id)
{
    return static_cast<std::uint32_t>(id & kSegmentOrdinalMask);
}

// Array positions. Internal, and never published -- see decision 2.
using SegmentIndex = std::uint32_t;
using NodeIndex = std::uint32_t;
using EdgeIndex = std::uint32_t;

inline constexpr SegmentIndex kNoSegment = 0xFFFFFFFFu;
inline constexpr NodeIndex kNoNode = 0xFFFFFFFFu;
// A string offset of 0 is "absent": offset 0 of the blob is always the empty
// string, so no real string can live there.
inline constexpr std::uint32_t kNoString = 0u;

// ---------------------------------------------------------------------------
// File layout
// ---------------------------------------------------------------------------

inline constexpr char kMagic[8] = { 'R', 'L', 'G', 'R', 'A', 'P', 'H', '1' };

// Bumped whenever a record layout changes. A mismatch is a loud refusal rather
// than a reinterpretation: the artifact is disposable (the build is offline and
// takes minutes), so there is never a reason to read an old one wrongly.
inline constexpr std::uint32_t kFormatVersion = 1;

enum class Section : std::uint32_t
{
    Nodes = 1,
    Segments = 2,
    Geometry = 3,
    Strings = 4,
    // CSR adjacency: one offset per node plus a trailing sentinel, then the
    // directed edges themselves.
    CsrOffsets = 5,
    CsrEdges = 6,
    // wayId -> segment index range, sorted by wayId. Decision 3.
    WayIndex = 7,
    // SegmentId -> segment index, sorted by id. Decision 2.
    SegmentIdIndex = 8,
    // Packed Hilbert R-tree over segment bounding boxes.
    SpatialIndex = 9,
    // (fromSegment, viaNode, toSegment) triples, sorted. Resolved at build time
    // through the way index -- see decision 3.
    TurnRestrictions = 10,
};

// THE ROUTING OVERLAY IS NOT A SECTION HERE. It is a sidecar, `<graph>.overlay`,
// and that is a decision rather than an oversight.
//
// The two artifacts have different lifetimes. A graph builds in under a minute
// and an overlay takes several; a graph without an overlay is completely usable
// (the router falls back to A*), and re-contracting must not mean rewriting a
// 900 MB file that did not change. Keeping them apart also means the overlay can
// be shipped, withheld or rebuilt per cost profile without touching the graph.
//
// What ties them together is not adjacency in a file but a checksum:
// routingChecksum() over the edges AND the turn restrictions, stored in the
// overlay header and verified on open. An overlay built against a different
// graph is refused rather than silently producing wrong routes.

#pragma pack(push, 1)

struct FileHeader
{
    char magic[8];
    std::uint32_t version;
    std::uint32_t sectionCount;

    std::uint32_t nodeCount;
    std::uint32_t segmentCount;
    std::uint32_t edgeCount;
    std::uint32_t geometryCount;

    // Coverage, in 1e-7 degrees.
    std::int32_t west;
    std::int32_t south;
    std::int32_t east;
    std::int32_t north;

    // Unix seconds. Zero when unknown. A driver reporting a missing road is
    // asking about this number.
    std::int64_t builtAtUnixS;

    std::uint64_t reserved[4];
};

struct SectionEntry
{
    std::uint32_t kind;
    std::uint32_t elementSize;
    std::uint64_t offset;
    std::uint64_t length;
};

// A junction, or the end of a way. 1e-7 degrees.
struct NodeRecord
{
    std::int32_t lat;
    std::int32_t lon;
};

// Segment flags.
inline constexpr std::uint16_t kFlagOnewayForward = 1u << 0;
inline constexpr std::uint16_t kFlagOnewayBackward = 1u << 1;
inline constexpr std::uint16_t kFlagHasPosted = 1u << 2;
inline constexpr std::uint16_t kFlagBridge = 1u << 3;
inline constexpr std::uint16_t kFlagTunnel = 1u << 4;
inline constexpr std::uint16_t kFlagRoundabout = 1u << 5;

struct SegmentRecord
{
    SegmentId id;
    std::int64_t osmWayId;

    // Into the geometry array, as coordinate PAIRS. Stored once; an edge that
    // runs the other way says so with its own flag rather than owning a
    // reversed copy. Decision 4.
    std::uint32_t geometryOffset;
    std::uint32_t geometryCount;

    NodeIndex fromNode;
    NodeIndex toNode;

    // Into the string blob. kNoString when absent.
    std::uint32_t nameOffset;
    std::uint32_t refOffset;

    // Ground length along the polyline, centimetres. A UInt32 reaches 42 949 km.
    std::uint32_t lengthCm;

    std::uint16_t postedSpeedKph;
    std::uint16_t freeFlowSpeedKph;
    std::uint16_t accessMask;
    std::uint16_t flags;

    std::uint8_t renderClass;
    std::uint8_t routeClass;
    std::uint8_t postedSource;
    std::uint8_t laneCount;
    std::int8_t layer;
    // Padded to 64 so a record never straddles a cache line, and so the next
    // field to arrive (a guidance tag, a partition cell) is free.
    std::uint8_t pad[7];
};

// One directed edge out of a node.
struct EdgeRecord
{
    SegmentIndex segment;
    NodeIndex target;
    // Traversal time at free-flow speed, in deciseconds. Cost profiles are
    // WEIGHT-ONLY -- none of them removes an edge -- which is what keeps a
    // shared partition (MLD) or a shared contraction (CH) possible at stage 6.
    std::uint32_t costDs;
    // True when this edge runs along the segment's stored geometry; false when
    // it runs against it.
    std::uint8_t forward;
    std::uint8_t pad[3];
};

struct WayIndexEntry
{
    std::int64_t wayId;
    SegmentIndex firstSegment;
    std::uint32_t segmentCount;
};

struct SegmentIdIndexEntry
{
    SegmentId id;
    SegmentIndex index;
    std::uint32_t pad;
};

// A packed Hilbert R-tree node. Leaves point at segments, inner nodes at the
// first child; the tree is complete and its level sizes are derivable, so no
// child count is stored.
// One turn restriction, already resolved from OSM way ids to segment indices.
//
// THERE IS NO EDGE-EXPANDED GRAPH IN THE FILE. The plan called for one, and
// building it turned out to be the wrong trade: expansion multiplies the edge
// count by average degree and would roughly triple an 887 MB SoCal artifact,
// to store something a search can derive for free. Instead the router's STATE
// is a directed edge rather than a node -- which is the same state space,
// materialised lazily -- and this table is consulted on each transition.
//
// The identities are what made that possible: because a segment id survives a
// rebuild and the way index maps an OSM way to its pieces, a restriction can be
// resolved once at build time rather than carried as way ids and re-resolved
// per query.
struct TurnRestrictionRecord
{
    SegmentIndex fromSegment;
    NodeIndex viaNode;
    SegmentIndex toSegment;
    // 0: this turn is prohibited (no_left_turn and friends).
    // 1: this turn is the ONLY one allowed from `fromSegment` at `viaNode`
    //    (only_straight_on and friends), so every other turn there is banned.
    std::uint8_t kind;
    std::uint8_t pad[3];
};

inline constexpr std::uint8_t kRestrictionProhibited = 0;
inline constexpr std::uint8_t kRestrictionOnly = 1;

struct RTreeNode
{
    std::int32_t west;
    std::int32_t south;
    std::int32_t east;
    std::int32_t north;
    // Segment index for a leaf, first-child node index for an inner node.
    std::uint32_t payload;
    std::uint32_t pad;
};

#pragma pack(pop)

// Fan-out of the packed R-tree. 16 keeps a node's children within a couple of
// cache lines and the tree shallow enough that a query touches few pages.
inline constexpr std::uint32_t kRTreeFanout = 16;

static_assert(sizeof(NodeRecord) == 8, "NodeRecord layout is on disk");
static_assert(sizeof(SegmentRecord) == 64, "SegmentRecord layout is on disk");
static_assert(sizeof(EdgeRecord) == 16, "EdgeRecord layout is on disk");
static_assert(sizeof(RTreeNode) == 24, "RTreeNode layout is on disk");
static_assert(sizeof(TurnRestrictionRecord) == 16, "TurnRestrictionRecord layout is on disk");

} // namespace road_graph

#endif // ROAD_GRAPH_FORMAT_H
