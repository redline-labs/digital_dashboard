// SPDX-License-Identifier: GPL-3.0-or-later
#include "road_graph/search.h"

#include <algorithm>
#include <queue>
#include <unordered_map>

namespace road_graph
{
namespace
{

struct Frontier
{
    double cost;
    NodeIndex node;

    bool operator>(const Frontier& other) const { return cost > other.cost; }
};

using Queue = std::priority_queue<Frontier, std::vector<Frontier>, std::greater<Frontier>>;

double lengthM(const Graph& graph, const EdgeRecord& edge)
{
    return static_cast<double>(graph.segments()[edge.segment].lengthCm) / 100.0;
}

} // namespace

std::optional<double> boundedDistance(const Graph& graph, NodeIndex from, NodeIndex to,
                                      double limitM)
{
    if (from == to)
    {
        return 0.0;
    }
    if (from >= graph.nodes().size() || to >= graph.nodes().size())
    {
        return std::nullopt;
    }

    // Straight-line distance is a floor on the driving distance, so a pair that
    // is already past the limit as the crow flies cannot possibly be under it
    // by road. Checking that first is what makes this cheap in the common case
    // of two candidates that are nowhere near each other.
    const NodeRecord& a = graph.nodes()[from];
    const NodeRecord& b = graph.nodes()[to];
    if (distanceM(a.lat, a.lon, b.lat, b.lon) > limitM)
    {
        return std::nullopt;
    }

    // A plain Dijkstra with a hash map rather than a full distance array: the
    // search is bounded to a few hundred metres, so it settles tens of nodes
    // out of millions, and allocating an array per call would dwarf the search.
    std::unordered_map<NodeIndex, double> best;
    Queue queue;
    queue.push({ 0.0, from });
    best.emplace(from, 0.0);

    while (!queue.empty())
    {
        const Frontier current = queue.top();
        queue.pop();

        if (current.node == to)
        {
            return current.cost;
        }
        if (current.cost > limitM)
        {
            // Every remaining frontier node is at least this far, so nothing
            // under the limit is left to find.
            return std::nullopt;
        }

        const auto found = best.find(current.node);
        if (found != best.end() && current.cost > found->second)
        {
            continue;
        }

        for (const EdgeRecord& edge : graph.edgesFrom(current.node))
        {
            const double next = current.cost + lengthM(graph, edge);
            if (next > limitM)
            {
                continue;
            }
            auto [entry, inserted] = best.try_emplace(edge.target, next);
            if (inserted || next < entry->second)
            {
                entry->second = next;
                queue.push({ next, edge.target });
            }
        }
    }

    return std::nullopt;
}

std::optional<Route> findRoute(const Graph& graph, NodeIndex from, NodeIndex to,
                               std::size_t maxSettled)
{
    if (from >= graph.nodes().size() || to >= graph.nodes().size())
    {
        return std::nullopt;
    }
    if (from == to)
    {
        return Route {};
    }

    // THE STATE IS A DIRECTED EDGE, NOT A NODE.
    //
    // A turn restriction is a property of a transition between two edges, and a
    // node-based search has nowhere to put it: by the time it reaches a
    // junction it has forgotten how it arrived. Searching over edges is exactly
    // the edge-expanded graph the textbooks build -- the same state space --
    // except derived as it goes rather than materialised. Expansion multiplies
    // the edge count by average degree, and materialising it would have roughly
    // tripled an 887 MB artifact to store what a binary search gives for free.
    const auto edges = graph.edges();
    const auto indexOfEdge = [&](const EdgeRecord& edge) {
        return static_cast<EdgeIndex>(&edge - edges.data());
    };

    // The fastest speed anything in the graph can go, for the heuristic. Taken
    // from the data rather than assumed: a graph of city streets should not be
    // searched with a motorway's optimism, which would make the heuristic weak
    // and the search wide.
    double fastestMps = 1.0;
    for (const SegmentRecord& segment : graph.segments())
    {
        fastestMps = std::max(fastestMps, segment.freeFlowSpeedKph / 3.6);
    }

    const NodeRecord& target = graph.nodes()[to];
    const auto heuristic = [&](NodeIndex node) {
        const NodeRecord& here = graph.nodes()[node];
        return distanceM(here.lat, here.lon, target.lat, target.lon) / fastestMps;
    };

    struct EdgeFrontier
    {
        double priority;
        EdgeIndex edge;

        bool operator>(const EdgeFrontier& other) const { return priority > other.priority; }
    };

    std::unordered_map<EdgeIndex, double> best;
    std::unordered_map<EdgeIndex, EdgeIndex> cameFrom;
    std::priority_queue<EdgeFrontier, std::vector<EdgeFrontier>, std::greater<EdgeFrontier>> queue;

    for (const EdgeRecord& edge : graph.edgesFrom(from))
    {
        const EdgeIndex index = indexOfEdge(edge);
        const double cost = static_cast<double>(edge.costDs) / 10.0;
        best.emplace(index, cost);
        queue.push({ cost + heuristic(edge.target), index });
    }

    std::size_t settled = 0;
    EdgeIndex arrival = 0;
    bool reached = false;

    while (!queue.empty())
    {
        const EdgeFrontier current = queue.top();
        queue.pop();

        const EdgeRecord& edge = edges[current.edge];
        const double cost = best[current.edge];
        if (current.priority > cost + heuristic(edge.target) + 1e-6)
        {
            continue;
        }

        if (edge.target == to)
        {
            arrival = current.edge;
            reached = true;
            break;
        }

        if (++settled > maxSettled)
        {
            // A continental query with no preprocessing overlay. Refused rather
            // than run for minutes: an honest "not answered" beats a hang, and
            // stage 6 is where this stops happening.
            return std::nullopt;
        }

        const auto outgoing = graph.edgesFrom(edge.target);
        for (const EdgeRecord& next : outgoing)
        {
            if (next.segment == edge.segment && outgoing.size() > 1)
            {
                // A U-turn on the same piece of road. Allowed only where it is
                // the sole option -- the end of a cul-de-sac -- because
                // otherwise a router will happily turn round mid-carriageway to
                // save a few seconds, which is a route no driver can follow.
                continue;
            }
            if (!graph.turnAllowed(edge.segment, edge.target, next.segment))
            {
                continue;
            }

            const EdgeIndex index = indexOfEdge(next);
            const double candidate = cost + static_cast<double>(next.costDs) / 10.0;

            auto [entry, inserted] = best.try_emplace(index, candidate);
            if (!inserted && candidate >= entry->second)
            {
                continue;
            }
            entry->second = candidate;
            cameFrom[index] = current.edge;
            queue.push({ candidate + heuristic(next.target), index });
        }
    }

    if (!reached)
    {
        return std::nullopt;
    }

    std::vector<EdgeIndex> path;
    EdgeIndex at = arrival;
    while (true)
    {
        path.push_back(at);
        const auto found = cameFrom.find(at);
        if (found == cameFrom.end())
        {
            break;
        }
        at = found->second;
    }
    std::reverse(path.begin(), path.end());

    Route route;
    route.durationS = best[arrival];
    for (const EdgeIndex index : path)
    {
        const EdgeRecord& edge = edges[index];
        const SegmentRecord& segment = graph.segments()[edge.segment];
        route.segments.push_back(edge.segment);
        route.distanceM += static_cast<double>(segment.lengthCm) / 100.0;

        // Geometry is stored once, in the segment's own direction; an edge that
        // runs the other way reverses HERE rather than owning a copy. That is
        // decision 4 in format.h, paid for at the one place it costs anything.
        const auto geometry = graph.geometryOf(segment);
        if (edge.forward != 0)
        {
            for (std::size_t i = 0; i + 1 < geometry.size(); i += 2)
            {
                route.geometry.push_back(geometry[i]);
                route.geometry.push_back(geometry[i + 1]);
            }
        }
        else
        {
            for (std::size_t i = geometry.size(); i >= 2; i -= 2)
            {
                route.geometry.push_back(geometry[i - 2]);
                route.geometry.push_back(geometry[i - 1]);
            }
        }
    }

    return route;
}

} // namespace road_graph
