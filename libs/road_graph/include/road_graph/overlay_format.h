// SPDX-License-Identifier: GPL-3.0-or-later
//
// The routing overlay: contraction hierarchies over the edge-expanded graph.
//
// WHY CH AND NOT MLD. The plan deferred this until there were numbers, and
// `map_build route` produced them on the real SoCal graph (5.0 M segments,
// 9.5 M directed edges), median / worst per straight-line band:
//
//     2-10 km      18 ms /   65 ms
//     10-30 km     71 ms /  139 ms
//     30-80 km    149 ms /  676 ms
//     80-200 km   482 ms /  629 ms
//
// The cost grows with roughly the square of the distance, because A* expands an
// ellipse and the ellipse's area does. Extrapolating that curve to a continental
// trip puts a single query in the minutes -- which is what the overlay exists to
// fix. Note also that the WORST case is already 676 ms at 30-80 km: the number
// that matters in a vehicle is the query that makes someone wait, not the median.
//
// Given those numbers, the choice between contraction hierarchies and
// multi-level Dijkstra comes down to what this system actually does:
//
//   - MLD's advantage is cheap RE-customization when weights change. There is no
//     live traffic here and the build is offline, so that advantage is bought
//     and never used. CH wins on the axis that is left: query speed.
//   - MLD's other advantage is one partition shared by every profile, where CH
//     needs an independent contraction per profile. That is a DISK cost, and
//     disk is the one resource this project has in surplus -- the user specified
//     low hundreds of GB, user-replaceable, on SSD.
//   - CH queries settle far fewer nodes, which matters most on the vehicle,
//     where the CPU is the scarce resource and the build machine is not.
//
// So: CH, one contraction per cost profile, each an additive artifact.
//
// WHY THE OVERLAY IS A SEPARATE FILE. It is built from the graph, long after the
// graph is written, and it is per-profile: folding it in would mean rewriting an
// 888 MB artifact to append to it, and doing that again for every profile. A
// separate file keeps the graph immutable and the overlays additive, which is
// what the plan asked for. The price is that the two can drift apart, so the
// overlay header carries the graph's identity and a mismatch is a loud refusal.
//
// WHY THE NODES ARE EDGES. Turn restrictions are properties of a TRANSITION
// between two directed edges and cannot be expressed on a node graph at all. So
// the hierarchy is contracted over the edge-expanded graph, where a node is one
// directed edge of the road graph and an arc is one legal transition. That makes
// the expanded node id and the road graph's edge index THE SAME NUMBER, which is
// what lets an unpacked shortcut fall out as a list of edges with no translation
// step -- and a translation step is exactly where a routing bug hides.
#ifndef ROAD_GRAPH_OVERLAY_FORMAT_H
#define ROAD_GRAPH_OVERLAY_FORMAT_H

#include <cstdint>
#include <span>

#include "road_graph/format.h"

namespace road_graph
{

inline constexpr char kOverlayMagic[8] = { 'R', 'L', 'G', 'O', 'V', 'R', 'L', '1' };

inline constexpr std::uint32_t kOverlayVersion = 1;

enum class OverlaySection : std::uint32_t
{
    // One per expanded node (= per directed edge), in contraction order. A node
    // contracted early has a low rank.
    Ranks = 1,
    // The upward search graph: arcs (u,v) with rank[u] < rank[v], stored at u.
    UpOffsets = 2,
    UpArcs = 3,
    // The downward search graph: arcs (u,v) with rank[u] > rank[v], stored at v
    // -- so a backward search from the target walks it the same way a forward
    // search walks the upward graph.
    DownOffsets = 4,
    DownArcs = 5,
    // Incoming edges per road-graph node, as CSR. The graph itself only stores
    // outgoing edges; a query needs "every edge that ENDS at the destination"
    // to seed the backward search, and scanning 9.5 M edges per query to find
    // them would cost more than the search it is seeding.
    IncomingOffsets = 6,
    IncomingEdges = 7,
};

#pragma pack(push, 1)

struct OverlayHeader
{
    char magic[8];
    std::uint32_t version;
    std::uint32_t sectionCount;

    // THE GRAPH THIS WAS BUILT FROM, copied verbatim from its header.
    //
    // An overlay applied to a different graph would not crash: shortcuts would
    // reference edge indices that exist and mean something else, and the router
    // would return fast, confident, wrong routes. So all four are checked and a
    // mismatch refuses to open.
    std::int64_t graphBuiltAtUnixS;
    std::uint32_t graphNodeCount;
    std::uint32_t graphSegmentCount;
    std::uint32_t graphEdgeCount;

    // A checksum over everything that decides a route: the edges AND the turn
    // restrictions.
    //
    // Counts and a build time are not enough on their own -- rebuild the same
    // extract with one rule changed and every count can land identically while
    // the costs differ. Restrictions have to be in it too, and that is not
    // hypothetical: two graphs differing ONLY by one banned turn have byte-
    // identical edge arrays, and a hierarchy built for one of them turns through
    // the ban in the other. Cheap to recompute on open -- one linear scan of
    // arrays that are about to be paged in anyway.
    std::uint64_t graphRoutingChecksum;

    // Expanded nodes = graphEdgeCount. Stored anyway so the file is readable
    // without the graph in hand.
    std::uint32_t expandedNodeCount;
    std::uint32_t pad;

    std::uint64_t originalArcCount;
    std::uint64_t shortcutCount;

    std::uint64_t reserved[4];
};

// An arc of the search graph: either a real transition or a shortcut standing
// for a path through one contracted node.
struct OverlayArc
{
    // The expanded node at the far end, which is a directed edge index.
    std::uint32_t target;
    // Deciseconds, matching EdgeRecord::costDs. Integer on purpose: a hierarchy
    // is only correct if the shortcut's weight equals the path's weight exactly,
    // and floating point sums do not promise that.
    std::uint32_t costDs;
    // The contracted node this shortcut stands in for, or kNoMiddle when the arc
    // is a real transition. Unpacking is what turns a route back into roads.
    std::uint32_t middle;
};

#pragma pack(pop)

inline constexpr std::uint32_t kNoMiddle = 0xFFFFFFFFu;

static_assert(sizeof(OverlayArc) == 12, "OverlayArc must stay 12 bytes");
static_assert(sizeof(OverlayHeader) == 100, "OverlayHeader layout changed; bump kOverlayVersion");

// The checksum an overlay carries and every open recomputes. Not a hash with
// cryptographic pretensions -- it defends against an honest mistake (the wrong
// file, a stale rebuild), not against an attacker.
std::uint64_t routingChecksum(std::span<const EdgeRecord> edges,
                              std::span<const TurnRestrictionRecord> restrictions);

} // namespace road_graph

#endif // ROAD_GRAPH_OVERLAY_FORMAT_H
