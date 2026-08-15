// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_build/rings.h"

#include <unordered_map>

namespace map_build
{
namespace
{

// Append `from` to `into`, dropping the joint so the shared node appears once.
void appendArc(std::vector<osm::Coord>& into, const std::vector<osm::Coord>& from, bool reversed)
{
    if (from.size() < 2)
    {
        return;
    }
    if (reversed)
    {
        // From the far end back, skipping the last pair -- it is the joint, and
        // it is already the last point of `into`.
        for (std::size_t i = from.size(); i >= 4; i -= 2)
        {
            into.push_back(from[i - 4]);
            into.push_back(from[i - 3]);
        }
    }
    else
    {
        for (std::size_t i = 2; i + 1 < from.size(); i += 2)
        {
            into.push_back(from[i]);
            into.push_back(from[i + 1]);
        }
    }
}

} // namespace

AssembledRings assembleRings(std::vector<RingArc>& arcs)
{
    AssembledRings out;

    // node id -> arcs that end there. An arc appears twice, once per end, and a
    // closed arc (a ring already, which is common for the simple island in a
    // lake) appears twice at the same node.
    std::unordered_multimap<std::int64_t, std::size_t> ends;
    ends.reserve(arcs.size() * 2);
    std::vector<std::uint8_t> used(arcs.size(), 0);

    for (std::size_t i = 0; i < arcs.size(); ++i)
    {
        if (arcs[i].geometry.size() < 4)
        {
            // Fewer than two points is not an arc; it cannot contribute an edge
            // to any ring.
            used[i] = 1;
            ++out.abandonedArcs;
            continue;
        }
        ends.emplace(arcs[i].firstNode, i);
        ends.emplace(arcs[i].lastNode, i);
    }

    for (std::size_t seed = 0; seed < arcs.size(); ++seed)
    {
        if (used[seed] != 0)
        {
            continue;
        }

        used[seed] = 1;
        std::vector<osm::Coord> ring = arcs[seed].geometry;
        const std::int64_t startNode = arcs[seed].firstNode;
        std::int64_t endNode = arcs[seed].lastNode;
        // The role of the ring is the role of the arc that started it. A
        // relation that disagrees with itself -- some members inner, some outer,
        // in one ring -- is malformed; taking the seed's role is arbitrary but
        // it is at least deterministic.
        const bool inner = arcs[seed].inner;
        std::vector<std::size_t> claimed { seed };

        while (endNode != startNode)
        {
            bool extended = false;
            auto range = ends.equal_range(endNode);
            for (auto it = range.first; it != range.second; ++it)
            {
                const std::size_t next = it->second;
                if (used[next] != 0)
                {
                    continue;
                }
                const bool reversed = arcs[next].lastNode == endNode;
                appendArc(ring, arcs[next].geometry, reversed);
                endNode = reversed ? arcs[next].firstNode : arcs[next].lastNode;
                used[next] = 1;
                claimed.push_back(next);
                extended = true;
                break;
            }
            if (!extended)
            {
                break;
            }
        }

        if (endNode != startNode || ring.size() < 6)
        {
            // Open, or too small to have an inside. DROPPED, not forced shut:
            // closing it across the gap would paint the missing piece as if it
            // were there, and a lake with a chord across it looks like a
            // rendering bug rather than missing data.
            out.abandonedArcs += static_cast<std::uint32_t>(claimed.size());
            continue;
        }

        // The closing point is implied, so the repeat is dropped if the data
        // carried one.
        if (ring.size() >= 8 && ring[0] == ring[ring.size() - 2] &&
            ring[1] == ring[ring.size() - 1])
        {
            ring.resize(ring.size() - 2);
        }

        (inner ? out.inner : out.outer).push_back(std::move(ring));
    }

    return out;
}

} // namespace map_build
