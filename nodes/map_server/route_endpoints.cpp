// SPDX-License-Identifier: GPL-3.0-or-later
#include "route_endpoints.h"

#include <algorithm>

namespace map_server
{

RouteEndpoints resolveEndpoints(const road_graph::Graph& graph, const road_graph::Match& start,
                                const road_graph::Match& finish,
                                std::optional<double> fromHeadingDeg)
{
    const road_graph::SegmentRecord& startSegment = graph.segments()[start.segment];
    const road_graph::SegmentRecord& endSegment = graph.segments()[finish.segment];

    RouteEndpoints out;

    // WHICH WAY THE VEHICLE IS POINTING decides which end of its own segment it
    // is driving towards. The heading was already used to rank candidates and
    // was then thrown away, so a vehicle travelling against the segment's
    // stored direction got routed from the junction BEHIND it -- a route that
    // opens with a U-turn the driver did not make, on exactly the divided
    // carriageways where supplying a heading mattered most.
    out.startForward = !fromHeadingDeg.has_value() ||
                       road_graph::bearingDeltaDeg(*fromHeadingDeg, start.bearingDeg) <= 90.0;
    out.startNode = out.startForward ? startSegment.toNode : startSegment.fromNode;

    // The destination end is NOT chosen the same way. There is no arrival
    // heading to choose it with, and choosing correctly means routing to both
    // ends of the segment and keeping the cheaper -- a second search, which
    // doubles the cost of every route. Left as the segment's start, which is
    // what it has always been; the error is bounded by the length of the final
    // segment and is at least visible in the distance now.
    out.endNode = endSegment.fromNode;

    // Clamped: offsetCm is accumulated from haversine leg lengths while
    // lengthCm is the builder's total, so the two can disagree by a centimetre
    // at the very end of a segment -- and unsigned arithmetic turns that into
    // a 42 000 km remainder.
    const std::uint32_t startOffset = std::min(start.offsetCm, startSegment.lengthCm);
    out.startRemainingM =
        (out.startForward ? (startSegment.lengthCm - startOffset) : startOffset) / 100.0;
    out.endLeadInM = std::min(finish.offsetCm, endSegment.lengthCm) / 100.0;

    return out;
}

} // namespace map_server
