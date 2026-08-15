// SPDX-License-Identifier: GPL-3.0-or-later
#include "road_graph/overlay.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace road_graph
{
namespace
{

constexpr std::uint64_t kInfinity = std::numeric_limits<std::uint64_t>::max();

struct Frontier
{
    std::uint64_t cost;
    std::uint32_t node;

    bool operator>(const Frontier& other) const { return cost > other.cost; }
};

// One direction of the bidirectional search.
//
// Both directions climb: the forward search walks the upward arcs, the backward
// search walks the downward arcs stored at their low end. Neither ever descends,
// which is why each settles a few hundred nodes rather than a few hundred
// thousand -- and why the meeting point is not necessarily on the shortest path
// until BOTH have run past it, which is what `best` below is for.
struct Direction
{
    std::unordered_map<std::uint32_t, std::uint64_t> cost;
    std::unordered_map<std::uint32_t, std::uint32_t> parent;
    std::priority_queue<Frontier, std::vector<Frontier>, std::greater<Frontier>> queue;
};

void relax(Direction& direction, std::uint32_t node, std::uint64_t candidate, std::uint32_t from)
{
    auto [entry, inserted] = direction.cost.try_emplace(node, candidate);
    if (!inserted)
    {
        if (candidate >= entry->second)
        {
            return;
        }
        entry->second = candidate;
    }
    direction.parent[node] = from;
    direction.queue.push({ candidate, node });
}

} // namespace

std::uint64_t routingChecksum(std::span<const EdgeRecord> edges,
                              std::span<const TurnRestrictionRecord> restrictions)
{
    // FNV-1a over the fields that decide a route. Deliberately NOT over the
    // padding: a struct's pad bytes are whatever the allocator left there, and
    // hashing them would make the checksum differ between two identical graphs.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        for (int byte = 0; byte < 8; ++byte)
        {
            hash ^= (value >> (byte * 8)) & 0xFFu;
            hash *= 1099511628211ULL;
        }
    };
    for (const EdgeRecord& edge : edges)
    {
        mix(edge.segment);
        mix(edge.target);
        mix(edge.costDs);
        mix(edge.forward);
    }
    for (const TurnRestrictionRecord& restriction : restrictions)
    {
        mix(restriction.fromSegment);
        mix(restriction.viaNode);
        mix(restriction.toSegment);
        mix(restriction.kind);
    }
    return hash;
}

struct OverlayMapping
{
    int fd { -1 };
    const std::byte* data { nullptr };
    std::size_t size { 0 };
};

Overlay::Overlay(Overlay&& other) noexcept
{
    *this = std::move(other);
}

Overlay& Overlay::operator=(Overlay&& other) noexcept
{
    if (this != &other)
    {
        mMapping = other.mMapping;
        mSize = other.mSize;
        mHeader = other.mHeader;
        mRanks = other.mRanks;
        mUpOffsets = other.mUpOffsets;
        mUpArcs = other.mUpArcs;
        mDownOffsets = other.mDownOffsets;
        mDownArcs = other.mDownArcs;
        mIncomingOffsets = other.mIncomingOffsets;
        mIncomingEdges = other.mIncomingEdges;
        other.mMapping = nullptr;
        other.mSize = 0;
    }
    return *this;
}

Overlay::~Overlay()
{
    if (mMapping != nullptr)
    {
        ::munmap(mMapping, mSize);
    }
}

Result<Overlay> Overlay::open(const std::filesystem::path& path, const Graph& graph)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return not_found(path.string());
    }

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return not_readable(path.string());
    }

    struct stat info {};
    if (::fstat(fd, &info) != 0)
    {
        ::close(fd);
        return not_readable(path.string() + ": cannot stat");
    }
    const auto size = static_cast<std::size_t>(info.st_size);
    if (size < sizeof(OverlayHeader))
    {
        ::close(fd);
        return not_a_graph(path.string() + ": too small to hold a header");
    }

    void* address = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (address == MAP_FAILED)
    {
        return not_readable(path.string() + ": cannot map");
    }

    Overlay overlay;
    overlay.mMapping = address;
    overlay.mSize = size;
    if (auto ok = overlay.bind(graph); !ok)
    {
        return std::unexpected(ok.error());
    }
    return overlay;
}

Result<void> Overlay::bind(const Graph& graph)
{
    const auto* base = static_cast<const std::byte*>(mMapping);
    mHeader = reinterpret_cast<const OverlayHeader*>(base);

    if (std::memcmp(mHeader->magic, kOverlayMagic, sizeof(kOverlayMagic)) != 0)
    {
        return not_a_graph("wrong magic");
    }
    if (mHeader->version != kOverlayVersion)
    {
        return version_mismatch("overlay is version " + std::to_string(mHeader->version) +
                                ", this build writes " + std::to_string(kOverlayVersion) +
                                "; rebuild it with tools/map_build");
    }

    // THE CHECK THAT MATTERS. An overlay built from another graph is not a
    // crash: every shortcut names an edge index that still exists and now means
    // a different road, so the router returns fast, confident nonsense.
    const FileHeader& graphHeader = graph.header();
    if (mHeader->graphBuiltAtUnixS != graphHeader.builtAtUnixS ||
        mHeader->graphNodeCount != graphHeader.nodeCount ||
        mHeader->graphSegmentCount != graphHeader.segmentCount ||
        mHeader->graphEdgeCount != graphHeader.edgeCount ||
        mHeader->graphRoutingChecksum !=
            routingChecksum(graph.edges(), graph.turnRestrictions()))
    {
        return version_mismatch("this overlay was built from a different graph; rebuild it");
    }

    const std::size_t tableBytes = sizeof(SectionEntry) * mHeader->sectionCount;
    if (sizeof(OverlayHeader) + tableBytes > mSize)
    {
        return malformed("section table runs past the end of the file");
    }
    const std::span<const SectionEntry> sections {
        reinterpret_cast<const SectionEntry*>(base + sizeof(OverlayHeader)), mHeader->sectionCount
    };

    const auto find = [&](OverlaySection kind) -> std::span<const std::byte> {
        for (const SectionEntry& entry : sections)
        {
            if (entry.kind == static_cast<std::uint32_t>(kind))
            {
                if (entry.offset + entry.length > mSize)
                {
                    return {};
                }
                return { base + entry.offset, entry.length };
            }
        }
        return {};
    };

    const auto bindU32 = [&](OverlaySection kind, std::span<const std::uint32_t>& out,
                             std::size_t expected) -> Result<void> {
        const auto bytes = find(kind);
        if (bytes.size() % sizeof(std::uint32_t) != 0)
        {
            return malformed("section " + std::to_string(static_cast<std::uint32_t>(kind)) +
                             " is not a whole number of elements");
        }
        out = { reinterpret_cast<const std::uint32_t*>(bytes.data()),
                bytes.size() / sizeof(std::uint32_t) };
        if (expected != 0 && out.size() != expected)
        {
            return malformed("section " + std::to_string(static_cast<std::uint32_t>(kind)) +
                             " has " + std::to_string(out.size()) + " elements, expected " +
                             std::to_string(expected));
        }
        return {};
    };

    const auto bindArcs = [&](OverlaySection kind,
                              std::span<const OverlayArc>& out) -> Result<void> {
        const auto bytes = find(kind);
        if (bytes.size() % sizeof(OverlayArc) != 0)
        {
            return malformed("arc section " + std::to_string(static_cast<std::uint32_t>(kind)) +
                             " is not a whole number of arcs");
        }
        out = { reinterpret_cast<const OverlayArc*>(bytes.data()),
                bytes.size() / sizeof(OverlayArc) };
        return {};
    };

    const std::size_t expanded = mHeader->expandedNodeCount;
    if (auto ok = bindU32(OverlaySection::Ranks, mRanks, expanded); !ok)
    {
        return ok;
    }
    if (auto ok = bindU32(OverlaySection::UpOffsets, mUpOffsets, expanded + 1); !ok)
    {
        return ok;
    }
    if (auto ok = bindU32(OverlaySection::DownOffsets, mDownOffsets, expanded + 1); !ok)
    {
        return ok;
    }
    if (auto ok = bindU32(OverlaySection::IncomingOffsets, mIncomingOffsets,
                          static_cast<std::size_t>(mHeader->graphNodeCount) + 1);
        !ok)
    {
        return ok;
    }
    if (auto ok = bindU32(OverlaySection::IncomingEdges, mIncomingEdges, expanded); !ok)
    {
        return ok;
    }
    if (auto ok = bindArcs(OverlaySection::UpArcs, mUpArcs); !ok)
    {
        return ok;
    }
    if (auto ok = bindArcs(OverlaySection::DownArcs, mDownArcs); !ok)
    {
        return ok;
    }

    return {};
}

std::span<const std::uint32_t> Overlay::incomingEdges(NodeIndex node) const
{
    if (static_cast<std::size_t>(node) + 1 >= mIncomingOffsets.size())
    {
        return {};
    }
    const std::uint32_t begin = mIncomingOffsets[node];
    const std::uint32_t end = mIncomingOffsets[node + 1];
    return mIncomingEdges.subspan(begin, end - begin);
}

std::optional<Route> findRouteVia(const Graph& graph, const Overlay& overlay, NodeIndex from,
                                  NodeIndex to)
{
    if (from == to)
    {
        return Route {};
    }

    Direction forward;
    Direction backward;

    // Seeding matches the plain router exactly: forward from every edge leaving
    // the origin at that edge's own cost, backward from every edge arriving at
    // the destination at zero. Get this wrong and the hierarchy is correct while
    // the answer is not.
    for (const EdgeRecord& edge : graph.edgesFrom(from))
    {
        const auto index = static_cast<std::uint32_t>(&edge - graph.edges().data());
        relax(forward, index, edge.costDs, index);
    }
    for (const std::uint32_t index : overlay.incomingEdges(to))
    {
        relax(backward, index, 0, index);
    }
    if (forward.cost.empty() || backward.cost.empty())
    {
        return std::nullopt;
    }

    std::uint64_t best = kInfinity;
    std::uint32_t meeting = 0;

    // Alternating, and NEITHER stops at the first meeting. A node settled by
    // both is a path, not necessarily the shortest one: the shortest may run
    // over a node one search has reached and the other has not settled yet. The
    // loop ends only when neither frontier can still beat `best`.
    while (!forward.queue.empty() || !backward.queue.empty())
    {
        const std::uint64_t forwardFront =
            forward.queue.empty() ? kInfinity : forward.queue.top().cost;
        const std::uint64_t backwardFront =
            backward.queue.empty() ? kInfinity : backward.queue.top().cost;
        if (forwardFront >= best && backwardFront >= best)
        {
            break;
        }

        Direction& direction = forwardFront <= backwardFront ? forward : backward;
        const Direction& other = forwardFront <= backwardFront ? backward : forward;
        const bool goingUp = forwardFront <= backwardFront;

        const Frontier current = direction.queue.top();
        direction.queue.pop();
        auto settledAt = direction.cost.find(current.node);
        if (settledAt == direction.cost.end() || current.cost > settledAt->second)
        {
            continue;
        }

        if (auto met = other.cost.find(current.node); met != other.cost.end())
        {
            const std::uint64_t total = current.cost + met->second;
            if (total < best)
            {
                best = total;
                meeting = current.node;
            }
        }

        const auto arcs = goingUp ? overlay.mUpArcs : overlay.mDownArcs;
        const auto offsets = goingUp ? overlay.mUpOffsets : overlay.mDownOffsets;
        const std::uint32_t begin = offsets[current.node];
        const std::uint32_t end = offsets[current.node + 1];
        for (std::uint32_t i = begin; i < end; ++i)
        {
            const OverlayArc& arc = arcs[i];
            relax(direction, arc.target, current.cost + arc.costDs, current.node);
        }
    }

    if (best == kInfinity)
    {
        return std::nullopt;
    }

    // ---- unpack ------------------------------------------------------------
    //
    // Expanded node ids ARE directed edge indices, so an unpacked path is a list
    // of edges with no translation. Shortcuts are expanded recursively through
    // the node each stands for.
    const auto unpack = [&](std::uint32_t u, std::uint32_t v, auto&& self,
                            std::vector<std::uint32_t>& out) -> void {
        // Find the arc, in whichever of the two graphs holds it.
        const OverlayArc* found = nullptr;
        const std::uint32_t upBegin = overlay.mUpOffsets[u];
        const std::uint32_t upEnd = overlay.mUpOffsets[u + 1];
        for (std::uint32_t i = upBegin; i < upEnd; ++i)
        {
            if (overlay.mUpArcs[i].target == v &&
                (found == nullptr || overlay.mUpArcs[i].costDs < found->costDs))
            {
                found = &overlay.mUpArcs[i];
            }
        }
        const std::uint32_t downBegin = overlay.mDownOffsets[v];
        const std::uint32_t downEnd = overlay.mDownOffsets[v + 1];
        for (std::uint32_t i = downBegin; i < downEnd; ++i)
        {
            if (overlay.mDownArcs[i].target == u &&
                (found == nullptr || overlay.mDownArcs[i].costDs < found->costDs))
            {
                found = &overlay.mDownArcs[i];
            }
        }

        if (found == nullptr || found->middle == kNoMiddle)
        {
            out.push_back(v);
            return;
        }
        self(u, found->middle, self, out);
        self(found->middle, v, self, out);
    };

    std::vector<std::uint32_t> edgesForward;
    {
        // Walk the forward parents back to a seed, unpacking each hop.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> hops;
        std::uint32_t node = meeting;
        while (true)
        {
            auto parent = forward.parent.find(node);
            if (parent == forward.parent.end() || parent->second == node)
            {
                break;
            }
            hops.emplace_back(parent->second, node);
            node = parent->second;
        }
        edgesForward.push_back(node);
        std::reverse(hops.begin(), hops.end());
        for (const auto& [u, v] : hops)
        {
            unpack(u, v, unpack, edgesForward);
        }
    }
    {
        std::uint32_t node = meeting;
        while (true)
        {
            auto parent = backward.parent.find(node);
            if (parent == backward.parent.end() || parent->second == node)
            {
                break;
            }
            // Backward parents point the way the vehicle travels, so each hop
            // unpacks in travel order and appends.
            unpack(node, parent->second, unpack, edgesForward);
            node = parent->second;
        }
    }

    // ---- the same Route the plain router builds ----------------------------
    Route route;
    route.segments.reserve(edgesForward.size());
    const auto edges = graph.edges();
    for (const std::uint32_t index : edgesForward)
    {
        const EdgeRecord& edge = edges[index];
        route.segments.push_back(edge.segment);
        route.durationS += static_cast<double>(edge.costDs) / 10.0;

        const SegmentRecord& segment = graph.segments()[edge.segment];
        route.distanceM += static_cast<double>(segment.lengthCm) / 100.0;

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
