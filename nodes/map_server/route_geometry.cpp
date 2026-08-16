// SPDX-License-Identifier: GPL-3.0-or-later
#include "route_geometry.h"

#include <algorithm>
#include <utility>

namespace map_server
{
namespace
{

// Perpendicular distance from `point` to the line through `a` and `b`, metres.
//
// projectOnto clamps to the segment, which is what Douglas-Peucker wants: a
// point beyond an end is as far away as that end, not as far as the infinite
// line would say.
double distanceToLineM(road_graph::Coord pLat, road_graph::Coord pLon, road_graph::Coord aLat,
                       road_graph::Coord aLon, road_graph::Coord bLat, road_graph::Coord bLon)
{
    return road_graph::projectOnto(pLat, pLon, aLat, aLon, bLat, bLon).distanceM;
}

// Iterative Douglas-Peucker over one run of coordinate pairs.
//
// Iterative rather than recursive: a segment's geometry comes off disk and a
// pathological one would otherwise be a stack overflow in a node that answers
// queries from the bus.
void simplifyRun(std::span<const road_graph::Coord> geometry, std::size_t first, std::size_t last,
                 double toleranceM, std::vector<bool>& keep)
{
    if (last <= first + 1)
    {
        return;
    }

    std::vector<std::pair<std::size_t, std::size_t>> pending;
    pending.emplace_back(first, last);

    while (!pending.empty())
    {
        const auto [from, to] = pending.back();
        pending.pop_back();
        if (to <= from + 1)
        {
            continue;
        }

        double worst = -1.0;
        std::size_t at = from;
        for (std::size_t i = from + 1; i < to; ++i)
        {
            const double away =
                distanceToLineM(geometry[i * 2], geometry[(i * 2) + 1], geometry[from * 2],
                                geometry[(from * 2) + 1], geometry[to * 2], geometry[(to * 2) + 1]);
            if (away > worst)
            {
                worst = away;
                at = i;
            }
        }

        if (worst > toleranceM)
        {
            keep[at] = true;
            pending.emplace_back(from, at);
            pending.emplace_back(at, to);
        }
    }
}

} // namespace

SimplifiedRoute simplifyPerSegment(std::span<const road_graph::Coord> geometry,
                                   std::span<const std::uint32_t> segmentStarts,
                                   double toleranceM)
{
    SimplifiedRoute out;

    const std::size_t points = geometry.size() / 2;
    if (points == 0 || segmentStarts.size() < 2 || toleranceM <= 0.0)
    {
        out.geometry.assign(geometry.begin(), geometry.end());
        out.segmentStarts.assign(segmentStarts.begin(), segmentStarts.end());
        return out;
    }

    std::vector<bool> keep(points, false);
    for (std::size_t s = 0; s + 1 < segmentStarts.size(); ++s)
    {
        const std::size_t first = segmentStarts[s];
        const std::size_t last = segmentStarts[s + 1];
        if (last <= first || last > points)
        {
            continue;
        }

        // Both ends of every run, always. They are the segment boundaries: drop
        // one and the runs stop meeting, and segmentStarts stops describing
        // where the roads change.
        keep[first] = true;
        keep[last - 1] = true;
        simplifyRun(geometry, first, last - 1, toleranceM, keep);
    }

    out.segmentStarts.reserve(segmentStarts.size());
    out.geometry.reserve(geometry.size());

    for (std::size_t s = 0; s + 1 < segmentStarts.size(); ++s)
    {
        out.segmentStarts.push_back(static_cast<std::uint32_t>(out.geometry.size() / 2));

        const std::size_t first = segmentStarts[s];
        const std::size_t last = std::min<std::size_t>(segmentStarts[s + 1], points);
        for (std::size_t i = first; i < last; ++i)
        {
            if (keep[i])
            {
                out.geometry.push_back(geometry[i * 2]);
                out.geometry.push_back(geometry[(i * 2) + 1]);
            }
        }
    }
    out.segmentStarts.push_back(static_cast<std::uint32_t>(out.geometry.size() / 2));

    return out;
}

} // namespace map_server
