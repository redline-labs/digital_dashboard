// SPDX-License-Identifier: GPL-3.0-or-later
#include "road_graph/contraction.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

#include <spdlog/spdlog.h>
#include <limits>
#include <queue>
#include <vector>

#include "road_graph/overlay_format.h"

namespace road_graph
{
namespace
{

constexpr std::uint32_t kInfinity = std::numeric_limits<std::uint32_t>::max();

// One arc of the working graph. Kept deliberately small: there are tens of
// millions of these and they are held twice, forward and backward.
struct Arc
{
    std::uint32_t target;
    std::uint32_t costDs;
    std::uint32_t middle;
};

// The graph being contracted.
//
// FLAT CSR FOR THE ORIGINAL ARCS, A LINKED LIST FOR THE SHORTCUTS. The obvious
// shape -- a vector of vectors -- costs 24 bytes of empty vector per node before
// a single arc exists, and on a continental graph that is two hundred million
// small heap allocations. Original arcs never change, so they live in one flat
// array; shortcuts are the only thing that grows, and they are chained through
// `nextOut`/`nextIn` off a per-node head. The result touches one allocation per
// batch instead of one per node.
struct Working
{
    // Immutable: every legal transition, as CSR.
    std::vector<std::uint32_t> outOffsets;
    std::vector<Arc> outArcs;
    std::vector<std::uint32_t> inOffsets;
    std::vector<Arc> inArcs;

    // Growable: shortcuts, chained per node.
    std::vector<Arc> extraOut;
    std::vector<std::uint32_t> nextOut;
    std::vector<std::uint32_t> headOut;
    std::vector<Arc> extraIn;
    std::vector<std::uint32_t> nextIn;
    std::vector<std::uint32_t> headIn;

    std::vector<std::uint8_t> contracted;
    std::vector<std::uint32_t> rank;

    static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

    // Visit every arc leaving (or entering) a node: the original ones, then any
    // shortcut that has been added since. Callers must not care about the order.
    template <typename Fn>
    void forEachOut(std::uint32_t node, Fn&& visit) const
    {
        for (std::uint32_t i = outOffsets[node]; i < outOffsets[node + 1]; ++i)
        {
            visit(outArcs[i]);
        }
        for (std::uint32_t i = headOut[node]; i != kNil; i = nextOut[i])
        {
            visit(extraOut[i]);
        }
    }

    template <typename Fn>
    void forEachIn(std::uint32_t node, Fn&& visit) const
    {
        for (std::uint32_t i = inOffsets[node]; i < inOffsets[node + 1]; ++i)
        {
            visit(inArcs[i]);
        }
        for (std::uint32_t i = headIn[node]; i != kNil; i = nextIn[i])
        {
            visit(extraIn[i]);
        }
    }

    // Add a shortcut, or lower the cost of one already present. Parallel arcs
    // are pointless in a shortest-path graph and would multiply every round.
    void addOut(std::uint32_t node, const Arc& arc)
    {
        for (std::uint32_t i = headOut[node]; i != kNil; i = nextOut[i])
        {
            if (extraOut[i].target == arc.target)
            {
                if (arc.costDs < extraOut[i].costDs)
                {
                    extraOut[i] = arc;
                }
                return;
            }
        }
        extraOut.push_back(arc);
        nextOut.push_back(headOut[node]);
        headOut[node] = static_cast<std::uint32_t>(extraOut.size() - 1);
    }

    void addIn(std::uint32_t node, const Arc& arc)
    {
        for (std::uint32_t i = headIn[node]; i != kNil; i = nextIn[i])
        {
            if (extraIn[i].target == arc.target)
            {
                if (arc.costDs < extraIn[i].costDs)
                {
                    extraIn[i] = arc;
                }
                return;
            }
        }
        extraIn.push_back(arc);
        nextIn.push_back(headIn[node]);
        headIn[node] = static_cast<std::uint32_t>(extraIn.size() - 1);
    }
};

// The two rules that decide whether a vehicle may go from one directed edge to
// the next. STATED ONCE, and identical to search.cpp's -- see the header.
bool transitionAllowed(const Graph& graph, const EdgeRecord& from, const EdgeRecord& next,
                       std::size_t outDegree)
{
    if (next.segment == from.segment && outDegree > 1)
    {
        // A U-turn on the same piece of road, allowed only where it is the sole
        // option -- the end of a cul-de-sac.
        return false;
    }
    return graph.turnAllowed(from.segment, from.target, next.segment);
}

// Does a path from `source` to `target` already exist that is no longer than
// `limit`, without going through `via`?
//
// This is the entire cost of building a hierarchy, and it is bounded twice: by
// settled nodes and by hops. A bound that stops early can only make it answer
// "no witness" when one existed, which adds a shortcut that was not needed --
// larger overlay, same routes. The opposite mistake, claiming a witness that
// does not exist, would drop a shortcut and make routes WRONG, so the search
// never relaxes past `limit`.
bool hasWitness(const Working& working, std::uint32_t source, std::uint32_t via,
                std::uint32_t target, std::uint32_t limit, const ContractionOptions& options,
                std::vector<std::uint32_t>& distance, std::vector<std::uint32_t>& touched)
{
    struct Item
    {
        std::uint32_t cost;
        std::uint32_t hops;
        std::uint32_t node;

        bool operator>(const Item& other) const { return cost > other.cost; }
    };

    for (const std::uint32_t node : touched)
    {
        distance[node] = kInfinity;
    }
    touched.clear();

    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    distance[source] = 0;
    touched.push_back(source);
    queue.push({ 0, 0, source });

    std::size_t settled = 0;
    while (!queue.empty())
    {
        const Item current = queue.top();
        queue.pop();
        if (current.cost > distance[current.node])
        {
            continue;
        }
        if (current.node == target)
        {
            return current.cost <= limit;
        }
        if (current.cost > limit || ++settled > options.witnessSettleLimit ||
            current.hops >= options.witnessHopLimit)
        {
            continue;
        }

        working.forEachOut(current.node, [&](const Arc& arc) {
            if (working.contracted[arc.target] != 0 || arc.target == via)
            {
                return;
            }
            const std::uint32_t candidate = current.cost + arc.costDs;
            if (candidate > limit || candidate >= distance[arc.target])
            {
                return;
            }
            if (distance[arc.target] == kInfinity)
            {
                touched.push_back(arc.target);
            }
            distance[arc.target] = candidate;
            queue.push({ candidate, current.hops + 1, arc.target });
        });
    }

    return false;
}

// What contracting `node` would cost, and the shortcuts it would need.
//
// Edge difference -- shortcuts added minus arcs removed -- is the classic
// ordering heuristic, and the "contracted neighbours" term is what stops the
// hierarchy growing in one place and leaving the rest of the map flat.
struct Simulation
{
    std::int64_t priority { 0 };
    std::vector<Arc> shortcuts;      // stored per source, parallel to sources
    std::vector<std::uint32_t> sources;
    std::uint64_t witnessSearches { 0 };
};

void simulate(const Working& working, std::uint32_t node, const ContractionOptions& options,
              std::vector<std::uint32_t>& distance, std::vector<std::uint32_t>& touched,
              std::vector<std::uint32_t>& contractedNeighbours, std::vector<Arc>& scratchIn,
              std::vector<Arc>& scratchOut, Simulation& out)
{
    out.shortcuts.clear();
    out.sources.clear();
    out.witnessSearches = 0;

    // Collected first, because forEachIn/forEachOut walk two lists and the
    // inner loop needs random access to the outgoing set.
    scratchIn.clear();
    scratchOut.clear();
    working.forEachIn(node, [&](const Arc& arc) {
        if (working.contracted[arc.target] == 0)
        {
            scratchIn.push_back(arc);
        }
    });
    working.forEachOut(node, [&](const Arc& arc) {
        if (working.contracted[arc.target] == 0)
        {
            scratchOut.push_back(arc);
        }
    });

    const auto removed = static_cast<std::int64_t>(scratchIn.size() + scratchOut.size());

    for (const Arc& incoming : scratchIn)
    {
        for (const Arc& outgoing : scratchOut)
        {
            if (incoming.target == outgoing.target)
            {
                // A shortcut from a node back to itself carries no information
                // and would make the search graph cyclic at one rank.
                continue;
            }

            const std::uint32_t limit = incoming.costDs + outgoing.costDs;
            ++out.witnessSearches;
            if (hasWitness(working, incoming.target, node, outgoing.target, limit, options,
                           distance, touched))
            {
                continue;
            }

            out.sources.push_back(incoming.target);
            out.shortcuts.push_back(Arc { outgoing.target, limit, node });
        }
    }

    out.priority = static_cast<std::int64_t>(out.shortcuts.size()) - removed +
                   2 * static_cast<std::int64_t>(contractedNeighbours[node]);
}

Result<void> writeSection(std::FILE* file, std::uint64_t& offset,
                          std::vector<std::pair<OverlaySection, std::pair<std::uint32_t, std::uint64_t>>>& table,
                          OverlaySection kind, std::uint32_t elementSize, const void* data,
                          std::uint64_t bytes)
{
    if (bytes != 0 && std::fwrite(data, 1, bytes, file) != bytes)
    {
        return not_readable("cannot write overlay section " +
                            std::to_string(static_cast<std::uint32_t>(kind)));
    }
    table.emplace_back(kind, std::make_pair(elementSize, offset));
    offset += bytes;
    return {};
}

} // namespace

Result<ContractionStats> buildOverlay(const Graph& graph, const std::filesystem::path& out,
                                      const ContractionOptions& options)
{
    const auto started = std::chrono::steady_clock::now();

    ContractionStats stats;
    const auto edges = graph.edges();
    const auto nodeCount = static_cast<std::uint32_t>(edges.size());
    stats.expandedNodes = nodeCount;

    if (nodeCount == 0)
    {
        return invalid_argument("the graph has no edges to contract");
    }

    // ---- 1. incoming edges per road-graph node -----------------------------
    //
    // Needed twice: to build the expanded graph's arcs, and by every query, to
    // seed the backward search from the destination.
    const auto roadNodes = static_cast<std::uint32_t>(graph.nodes().size());
    std::vector<std::uint32_t> incomingOffsets(static_cast<std::size_t>(roadNodes) + 1, 0);
    for (const EdgeRecord& edge : edges)
    {
        ++incomingOffsets[static_cast<std::size_t>(edge.target) + 1];
    }
    for (std::size_t i = 1; i < incomingOffsets.size(); ++i)
    {
        incomingOffsets[i] += incomingOffsets[i - 1];
    }
    std::vector<std::uint32_t> incomingEdges(edges.size(), 0);
    {
        std::vector<std::uint32_t> cursor(incomingOffsets.begin(), incomingOffsets.end() - 1);
        for (std::size_t i = 0; i < edges.size(); ++i)
        {
            incomingEdges[cursor[edges[i].target]++] = static_cast<std::uint32_t>(i);
        }
    }

    SPDLOG_INFO("[overlay] expanded graph: {} nodes, one per directed edge", nodeCount);

    // ---- 2. the expanded graph --------------------------------------------
    //
    // Counted first, then filled. Two passes over the graph cost less than one
    // pass that reallocates: at this size a growing array spends most of its
    // time copying itself.
    Working working;
    working.contracted.assign(nodeCount, 0);
    working.rank.assign(nodeCount, 0);
    working.headOut.assign(nodeCount, Working::kNil);
    working.headIn.assign(nodeCount, Working::kNil);
    working.outOffsets.assign(static_cast<std::size_t>(nodeCount) + 1, 0);
    working.inOffsets.assign(static_cast<std::size_t>(nodeCount) + 1, 0);

    const auto forEachTransition = [&](auto&& visit) {
        for (std::uint32_t index = 0; index < nodeCount; ++index)
        {
            const EdgeRecord& edge = edges[index];
            const auto outgoing = graph.edgesFrom(edge.target);
            for (const EdgeRecord& next : outgoing)
            {
                if (!transitionAllowed(graph, edge, next, outgoing.size()))
                {
                    continue;
                }
                visit(index, static_cast<std::uint32_t>(&next - edges.data()), next.costDs);
            }
        }
    };

    forEachTransition([&](std::uint32_t from, std::uint32_t to, std::uint32_t) {
        ++working.outOffsets[static_cast<std::size_t>(from) + 1];
        ++working.inOffsets[static_cast<std::size_t>(to) + 1];
        ++stats.originalArcs;
    });
    for (std::size_t i = 1; i < working.outOffsets.size(); ++i)
    {
        working.outOffsets[i] += working.outOffsets[i - 1];
        working.inOffsets[i] += working.inOffsets[i - 1];
    }
    working.outArcs.resize(stats.originalArcs);
    working.inArcs.resize(stats.originalArcs);
    {
        std::vector<std::uint32_t> outCursor(working.outOffsets.begin(),
                                             working.outOffsets.end() - 1);
        std::vector<std::uint32_t> inCursor(working.inOffsets.begin(), working.inOffsets.end() - 1);
        forEachTransition([&](std::uint32_t from, std::uint32_t to, std::uint32_t costDs) {
            working.outArcs[outCursor[from]++] = Arc { to, costDs, kNoMiddle };
            working.inArcs[inCursor[to]++] = Arc { from, costDs, kNoMiddle };
        });
    }

    SPDLOG_INFO("[overlay] {} legal transitions", stats.originalArcs);

    // ---- 3. contraction ----------------------------------------------------
    struct Queued
    {
        std::int64_t priority;
        std::uint32_t node;

        bool operator>(const Queued& other) const
        {
            // Ties broken by index so two runs on the same graph produce the
            // same hierarchy. A build that is not reproducible cannot be
            // compared against itself after a change.
            return priority != other.priority ? priority > other.priority : node > other.node;
        }
    };

    std::vector<std::uint32_t> distance(nodeCount, kInfinity);
    std::vector<std::uint32_t> touched;
    std::vector<std::uint32_t> contractedNeighbours(nodeCount, 0);
    std::vector<Arc> scratchIn;
    std::vector<Arc> scratchOut;
    Simulation simulation;

    std::priority_queue<Queued, std::vector<Queued>, std::greater<Queued>> queue;

    // THE FIRST ORDERING IS A GUESS, ON PURPOSE.
    //
    // Simulating every node properly means a witness search per in/out pair --
    // on this graph, tens of millions of bounded Dijkstras before a single node
    // is contracted, which is most of a build spent on numbers that go stale the
    // moment contraction starts. So the initial priority is the local estimate
    // (pairs that would need a shortcut, minus arcs that would disappear) and
    // the real simulation happens lazily, when a node reaches the front of the
    // queue and is about to be contracted. That is the only place the number has
    // to be right.
    {
        std::vector<Queued> initial;
        initial.reserve(nodeCount);
        for (std::uint32_t node = 0; node < nodeCount; ++node)
        {
            const auto inDegree = static_cast<std::int64_t>(working.inOffsets[node + 1] -
                                                            working.inOffsets[node]);
            const auto outDegree = static_cast<std::int64_t>(working.outOffsets[node + 1] -
                                                             working.outOffsets[node]);
            initial.push_back({ inDegree * outDegree - inDegree - outDegree, node });
        }
        queue = std::priority_queue<Queued, std::vector<Queued>, std::greater<Queued>>(
            std::greater<Queued> {}, std::move(initial));
    }

    SPDLOG_INFO("[overlay] initial ordering done, contracting");

    // Nodes above this many are left in the core. A fraction of zero would mean
    // no hierarchy at all, which is a legal thing to ask for and is what the
    // test uses to check the core path on its own.
    const auto contractLimit = static_cast<std::uint32_t>(
        static_cast<double>(nodeCount) * std::clamp(options.stopAtFraction, 0.0, 1.0));

    std::uint32_t rank = 0;
    while (!queue.empty() && rank < contractLimit)
    {
        const Queued top = queue.top();
        queue.pop();
        if (working.contracted[top.node] != 0)
        {
            continue;
        }

        // LAZY UPDATE. Contracting a node changes its neighbours' priorities,
        // and eagerly updating every one of them is most of the build time. So
        // the priority is recomputed only when the node reaches the front, and
        // it is contracted only if it is still the smallest.
        simulate(working, top.node, options, distance, touched, contractedNeighbours, scratchIn,
                 scratchOut, simulation);
        stats.witnessSearches += simulation.witnessSearches;
        if (!queue.empty() && simulation.priority > queue.top().priority)
        {
            queue.push({ simulation.priority, top.node });
            continue;
        }

        working.contracted[top.node] = 1;
        working.rank[top.node] = rank++;

        for (std::size_t i = 0; i < simulation.shortcuts.size(); ++i)
        {
            const std::uint32_t source = simulation.sources[i];
            const Arc& shortcut = simulation.shortcuts[i];
            working.addOut(source, shortcut);
            working.addIn(shortcut.target, Arc { source, shortcut.costDs, shortcut.middle });
            ++stats.shortcuts;
        }

        working.forEachOut(top.node, [&](const Arc& arc) { ++contractedNeighbours[arc.target]; });
        working.forEachIn(top.node, [&](const Arc& arc) { ++contractedNeighbours[arc.target]; });

        if (options.progressEvery != 0 && rank % options.progressEvery == 0)
        {
            SPDLOG_INFO("[overlay] contracted {}/{}, {} shortcuts", rank, nodeCount,
                        stats.shortcuts);
        }
    }

    // EVERY UNCONTRACTED NODE GETS THE SAME RANK, and that is what makes the
    // core searchable. Arcs between two nodes of equal rank land in BOTH search
    // graphs below, so the forward and backward searches can each move freely
    // inside the core -- which is precisely a plain bidirectional Dijkstra over
    // it, entered from the hierarchy and left the same way.
    for (std::uint32_t node = 0; node < nodeCount; ++node)
    {
        if (working.contracted[node] == 0)
        {
            working.rank[node] = nodeCount;
            ++stats.coreNodes;
        }
    }

    SPDLOG_INFO("[overlay] contraction done: {} shortcuts, {} nodes left in the core",
                stats.shortcuts, stats.coreNodes);

    // ---- 4. the two search graphs -----------------------------------------
    //
    // An arc goes UP if its far end outranks its near end. The backward search
    // walks the downward arcs stored at their LOW end, so both searches read
    // their graph the same way and neither needs a reversal at query time.
    std::vector<std::uint32_t> upOffsets(static_cast<std::size_t>(nodeCount) + 1, 0);
    std::vector<std::uint32_t> downOffsets(static_cast<std::size_t>(nodeCount) + 1, 0);
    for (std::uint32_t node = 0; node < nodeCount; ++node)
    {
        // GREATER OR EQUAL, not greater. Equal means both ends are in the core,
        // and such an arc has to be walkable in both directions or the core is
        // not searchable at all -- routes that cross it would simply not be
        // found, which looks like a disconnected map.
        working.forEachOut(node, [&](const Arc& arc) {
            if (working.rank[arc.target] >= working.rank[node])
            {
                ++upOffsets[static_cast<std::size_t>(node) + 1];
            }
        });
        working.forEachIn(node, [&](const Arc& arc) {
            if (working.rank[arc.target] >= working.rank[node])
            {
                ++downOffsets[static_cast<std::size_t>(node) + 1];
            }
        });
    }
    for (std::size_t i = 1; i < upOffsets.size(); ++i)
    {
        upOffsets[i] += upOffsets[i - 1];
        downOffsets[i] += downOffsets[i - 1];
    }

    std::vector<OverlayArc> upArcs(upOffsets.back());
    std::vector<OverlayArc> downArcs(downOffsets.back());
    {
        std::vector<std::uint32_t> upCursor(upOffsets.begin(), upOffsets.end() - 1);
        std::vector<std::uint32_t> downCursor(downOffsets.begin(), downOffsets.end() - 1);
        for (std::uint32_t node = 0; node < nodeCount; ++node)
        {
            working.forEachOut(node, [&](const Arc& arc) {
                if (working.rank[arc.target] >= working.rank[node])
                {
                    upArcs[upCursor[node]++] = OverlayArc { arc.target, arc.costDs, arc.middle };
                }
            });
            working.forEachIn(node, [&](const Arc& arc) {
                if (working.rank[arc.target] >= working.rank[node])
                {
                    downArcs[downCursor[node]++] =
                        OverlayArc { arc.target, arc.costDs, arc.middle };
                }
            });
        }
    }

    // ---- 5. write ----------------------------------------------------------
    std::FILE* file = std::fopen(out.c_str(), "wb");
    if (file == nullptr)
    {
        return not_readable("cannot create " + out.string());
    }

    OverlayHeader header {};
    std::copy(std::begin(kOverlayMagic), std::end(kOverlayMagic), std::begin(header.magic));
    header.version = kOverlayVersion;
    header.sectionCount = 7;
    header.graphBuiltAtUnixS = graph.header().builtAtUnixS;
    header.graphNodeCount = graph.header().nodeCount;
    header.graphSegmentCount = graph.header().segmentCount;
    header.graphEdgeCount = graph.header().edgeCount;
    header.graphRoutingChecksum = routingChecksum(edges, graph.turnRestrictions());
    header.expandedNodeCount = nodeCount;
    header.originalArcCount = stats.originalArcs;
    header.shortcutCount = stats.shortcuts;

    const std::uint64_t tableBytes =
        static_cast<std::uint64_t>(header.sectionCount) * sizeof(SectionEntry);
    std::uint64_t offset = sizeof(OverlayHeader) + tableBytes;

    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
    {
        std::fclose(file);
        return not_readable("cannot reserve the overlay section table");
    }

    std::vector<std::pair<OverlaySection, std::pair<std::uint32_t, std::uint64_t>>> table;
    std::vector<std::uint64_t> lengths;

    const auto put = [&](OverlaySection kind, std::uint32_t elementSize, const void* data,
                         std::uint64_t bytes) -> Result<void> {
        auto ok = writeSection(file, offset, table, kind, elementSize, data, bytes);
        lengths.push_back(bytes);
        return ok;
    };

    if (auto ok = put(OverlaySection::Ranks, sizeof(std::uint32_t), working.rank.data(),
                      working.rank.size() * sizeof(std::uint32_t));
        !ok)
    {
        std::fclose(file);
        return std::unexpected(ok.error());
    }
    if (auto ok = put(OverlaySection::UpOffsets, sizeof(std::uint32_t), upOffsets.data(),
                      upOffsets.size() * sizeof(std::uint32_t));
        !ok)
    {
        std::fclose(file);
        return std::unexpected(ok.error());
    }
    if (auto ok = put(OverlaySection::UpArcs, sizeof(OverlayArc), upArcs.data(),
                      upArcs.size() * sizeof(OverlayArc));
        !ok)
    {
        std::fclose(file);
        return std::unexpected(ok.error());
    }
    if (auto ok = put(OverlaySection::DownOffsets, sizeof(std::uint32_t), downOffsets.data(),
                      downOffsets.size() * sizeof(std::uint32_t));
        !ok)
    {
        std::fclose(file);
        return std::unexpected(ok.error());
    }
    if (auto ok = put(OverlaySection::DownArcs, sizeof(OverlayArc), downArcs.data(),
                      downArcs.size() * sizeof(OverlayArc));
        !ok)
    {
        std::fclose(file);
        return std::unexpected(ok.error());
    }
    if (auto ok = put(OverlaySection::IncomingOffsets, sizeof(std::uint32_t), incomingOffsets.data(),
                      incomingOffsets.size() * sizeof(std::uint32_t));
        !ok)
    {
        std::fclose(file);
        return std::unexpected(ok.error());
    }
    if (auto ok = put(OverlaySection::IncomingEdges, sizeof(std::uint32_t), incomingEdges.data(),
                      incomingEdges.size() * sizeof(std::uint32_t));
        !ok)
    {
        std::fclose(file);
        return std::unexpected(ok.error());
    }

    std::vector<SectionEntry> entries;
    entries.reserve(table.size());
    for (std::size_t i = 0; i < table.size(); ++i)
    {
        SectionEntry entry {};
        entry.kind = static_cast<std::uint32_t>(table[i].first);
        entry.elementSize = table[i].second.first;
        entry.offset = table[i].second.second;
        entry.length = lengths[i];
        entries.push_back(entry);
    }

    if (std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, file) != 1 ||
        std::fwrite(entries.data(), sizeof(SectionEntry), entries.size(), file) != entries.size())
    {
        std::fclose(file);
        return not_readable("cannot write the overlay header");
    }

    stats.bytes = offset;
    if (std::fclose(file) != 0)
    {
        return not_readable("cannot close " + out.string());
    }

    stats.buildSeconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              started)
            .count() /
        1000.0;
    return stats;
}

} // namespace road_graph
