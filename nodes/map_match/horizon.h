// SPDX-License-Identifier: GPL-3.0-or-later
//
// Building the lookahead from a match.
//
// v1 emits ONE path: the segment the vehicle is on plus its unambiguous
// continuation -- as far as the road goes without a choice. Where the road
// forks, the path ends. Branches with probabilities are the next thing this
// grows, and they grow as extra HorizonPath entries rather than as a change to
// anything already here.
//
// Zenoh-free and capnp-free, like matcher.h, so what the horizon CONTAINS can
// be tested without a bus. services.cpp does nothing but copy this onto the
// wire.
#ifndef MAP_MATCH_HORIZON_H
#define MAP_MATCH_HORIZON_H

#include <cstdint>
#include <string>
#include <vector>

#include "road_graph/graph.h"

namespace map_match
{

// One stretch of the path, from one graph segment.
//
// The horizon is a sequence of these; every profile a consumer reads is derived
// from them, which is why the offsets are computed once here rather than per
// profile type.
struct HorizonRun
{
    road_graph::SegmentIndex segment { road_graph::kNoSegment };
    road_graph::SegmentId segmentId { 0 };

    // Along the path, not along the segment.
    std::uint32_t startOffsetCm { 0 };
    std::uint32_t endOffsetCm { 0 };

    // True when the path runs along the segment's stored geometry.
    bool forward { true };
};

struct Horizon
{
    // Where the vehicle is, as an offset into the path.
    std::uint32_t positionOffsetCm { 0 };
    std::uint32_t lengthCm { 0 };
    std::vector<HorizonRun> runs;
};

// Build the horizon ahead of a match.
//
// Follows the road while the choice is forced: at each junction, if exactly one
// edge leaves that is not the way we came, take it. At a fork, stop -- guessing
// which way a driver will go is what the branch probabilities are for, and a
// horizon that guesses wrong is worse than one that stops.
Horizon buildHorizon(const road_graph::Graph& graph, road_graph::SegmentIndex segment,
                     std::uint32_t offsetCm, bool forward, std::uint32_t lookaheadCm);

} // namespace map_match

#endif // MAP_MATCH_HORIZON_H
