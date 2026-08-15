// SPDX-License-Identifier: GPL-3.0-or-later
#include "horizon.h"

#include <algorithm>

namespace map_match
{

Horizon buildHorizon(const road_graph::Graph& graph, road_graph::SegmentIndex segment,
                     std::uint32_t offsetCm, bool forward, std::uint32_t lookaheadCm)
{
    Horizon horizon;
    if (segment >= graph.segments().size())
    {
        return horizon;
    }

    const road_graph::SegmentRecord& first = graph.segments()[segment];

    // The path starts at the vehicle, not at the start of the segment. Anything
    // behind it is not lookahead, and including it would make every offset a
    // consumer reads relative to a point the vehicle has already passed.
    const std::uint32_t behindCm = forward ? offsetCm : first.lengthCm - offsetCm;
    const std::uint32_t aheadOnFirst = first.lengthCm - behindCm;

    HorizonRun start;
    start.segment = segment;
    start.segmentId = first.id;
    start.startOffsetCm = 0;
    start.endOffsetCm = std::min(aheadOnFirst, lookaheadCm);
    start.forward = forward;
    horizon.runs.push_back(start);

    std::uint32_t total = start.endOffsetCm;
    horizon.positionOffsetCm = 0;

    road_graph::NodeIndex at = forward ? first.toNode : first.fromNode;
    road_graph::SegmentIndex previous = segment;

    // Follow while the choice is forced. `guard` bounds a pathological loop --
    // a roundabout with no exit in the data would otherwise walk forever.
    for (int guard = 0; guard < 512 && total < lookaheadCm; ++guard)
    {
        const auto outgoing = graph.edgesFrom(at);

        const road_graph::EdgeRecord* only = nullptr;
        int choices = 0;
        for (const road_graph::EdgeRecord& edge : outgoing)
        {
            if (edge.segment == previous)
            {
                // Not a choice: that is the way we came.
                continue;
            }
            ++choices;
            only = &edge;
        }

        if (choices != 1 || only == nullptr)
        {
            // A fork, or a dead end. Stopping is the honest answer: guessing
            // which way a driver will go is what branch probabilities are for,
            // and a horizon that guesses wrong is worse than a short one.
            break;
        }

        const road_graph::SegmentRecord& record = graph.segments()[only->segment];

        HorizonRun run;
        run.segment = only->segment;
        run.segmentId = record.id;
        run.startOffsetCm = total;
        run.endOffsetCm = std::min(total + record.lengthCm, lookaheadCm);
        run.forward = only->forward != 0;
        horizon.runs.push_back(run);

        total = run.endOffsetCm;
        previous = only->segment;
        at = only->target;
    }

    horizon.lengthCm = total;
    return horizon;
}

} // namespace map_match
