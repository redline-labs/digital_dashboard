// SPDX-License-Identifier: GPL-3.0-or-later
#include "road_graph/graph.h"

#include <algorithm>
#include <ranges>
#include <cstring>
#include <limits>

#include "map_rules/classification.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace road_graph
{
namespace
{

// How much worse a class is than a motorway, in "equivalent metres".
//
// Distance alone puts a vehicle on whatever happens to be nearest, and in a
// business park that is a parking aisle five metres away rather than the road
// it is driving on. Every serious matcher biases by class for exactly this
// reason.
//
// The numbers are deliberately SMALL for real roads and large for the ones a
// vehicle is rarely on: a primary road loses to a residential street that is
// four metres nearer, but a service road has to be twenty metres nearer to beat
// either. What this must never do is override a clear distance answer -- a car
// genuinely in a car park should still match the car park.
double classPenaltyM(std::uint8_t routeClass)
{
    switch (static_cast<map_rules::RouteClass>(routeClass))
    {
        case map_rules::RouteClass::None:
            return 1000.0;
        case map_rules::RouteClass::Motorway:
            return 0.0;
        case map_rules::RouteClass::Trunk:
            return 1.0;
        case map_rules::RouteClass::Primary:
            return 2.0;
        case map_rules::RouteClass::Secondary:
            return 3.0;
        case map_rules::RouteClass::Tertiary:
            return 4.0;
        case map_rules::RouteClass::Minor:
            return 6.0;
        case map_rules::RouteClass::Service:
            return 20.0;
        case map_rules::RouteClass::Track:
            return 25.0;
        case map_rules::RouteClass::Path:
            return 30.0;
        case map_rules::RouteClass::Pedestrian:
            return 30.0;
        case map_rules::RouteClass::Ferry:
            return 50.0;
    }

    // After the switch rather than in a default:, so adding a class stays a
    // compile error under -Wswitch-enum.
    return 1000.0;
}

} // namespace

struct Graph::Mapping
{
    int fd { -1 };
    const std::byte* data { nullptr };
    std::size_t size { 0 };

    ~Mapping()
    {
        if (data != nullptr)
        {
            ::munmap(const_cast<std::byte*>(data), size);
        }
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
};

Graph::Graph(Graph&&) noexcept = default;
Graph& Graph::operator=(Graph&&) noexcept = default;
Graph::~Graph() = default;

Result<Graph> Graph::open(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return not_found(path.string());
    }

    auto mapping = std::make_unique<Mapping>();
    mapping->fd = ::open(path.c_str(), O_RDONLY);
    if (mapping->fd < 0)
    {
        return not_readable(path.string());
    }

    struct stat info {};
    if (::fstat(mapping->fd, &info) != 0)
    {
        return not_readable(path.string() + ": cannot stat");
    }
    mapping->size = static_cast<std::size_t>(info.st_size);
    if (mapping->size < sizeof(FileHeader))
    {
        return not_a_graph(path.string() + ": too small to hold a header");
    }

    void* address = ::mmap(nullptr, mapping->size, PROT_READ, MAP_PRIVATE, mapping->fd, 0);
    if (address == MAP_FAILED)
    {
        return not_readable(path.string() + ": cannot map");
    }
    mapping->data = static_cast<const std::byte*>(address);

    Graph graph;
    graph.mMapping = std::move(mapping);
    if (auto ok = graph.bind(); !ok)
    {
        return std::unexpected(ok.error());
    }
    return graph;
}

std::span<const std::byte> Graph::section(Section kind) const
{
    for (const SectionEntry& entry : mSections)
    {
        if (entry.kind == static_cast<std::uint32_t>(kind))
        {
            if (entry.offset + entry.length > mMapping->size)
            {
                return {};
            }
            return { mMapping->data + entry.offset, entry.length };
        }
    }
    return {};
}

Result<void> Graph::bind()
{
    mHeader = reinterpret_cast<const FileHeader*>(mMapping->data);

    if (std::memcmp(mHeader->magic, kMagic, sizeof(kMagic)) != 0)
    {
        return not_a_graph("wrong magic");
    }
    if (mHeader->version != kFormatVersion)
    {
        return version_mismatch("file is version " + std::to_string(mHeader->version) +
                                ", this build writes " + std::to_string(kFormatVersion) +
                                "; rebuild it with tools/map_build");
    }

    const std::size_t tableBytes = sizeof(SectionEntry) * mHeader->sectionCount;
    if (sizeof(FileHeader) + tableBytes > mMapping->size)
    {
        return malformed("section table runs past the end of the file");
    }
    mSections = { reinterpret_cast<const SectionEntry*>(mMapping->data + sizeof(FileHeader)),
                  mHeader->sectionCount };

    // Every required section, bound and length-checked. A section whose length
    // is not a multiple of its element size would otherwise yield a span with a
    // truncated last element, which reads as data.
    const auto bindSpan = [&]<typename T>(Section kind, std::span<const T>& out,
                                          const char* name) -> Result<void> {
        const auto bytes = section(kind);
        if (bytes.empty() && kind != Section::Strings)
        {
            return malformed(std::string("missing section: ") + name);
        }
        if (bytes.size() % sizeof(T) != 0)
        {
            return malformed(std::string(name) + " section is " + std::to_string(bytes.size()) +
                             " bytes, not a multiple of " + std::to_string(sizeof(T)));
        }
        out = { reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T) };
        return {};
    };

    if (auto ok = bindSpan(Section::Nodes, mNodes, "nodes"); !ok)
    {
        return ok;
    }
    if (auto ok = bindSpan(Section::Segments, mSegments, "segments"); !ok)
    {
        return ok;
    }
    if (auto ok = bindSpan(Section::CsrEdges, mEdges, "edges"); !ok)
    {
        return ok;
    }
    if (auto ok = bindSpan(Section::Geometry, mGeometry, "geometry"); !ok)
    {
        return ok;
    }
    if (auto ok = bindSpan(Section::CsrOffsets, mCsrOffsets, "csr offsets"); !ok)
    {
        return ok;
    }
    if (auto ok = bindSpan(Section::WayIndex, mWayIndex, "way index"); !ok)
    {
        return ok;
    }
    if (auto ok = bindSpan(Section::SegmentIdIndex, mIdIndex, "segment id index"); !ok)
    {
        return ok;
    }
    if (auto ok = bindSpan(Section::SpatialIndex, mRTree, "spatial index"); !ok)
    {
        return ok;
    }

    // Optional: a graph built before restrictions existed, or from an extract
    // with none, simply has no such section. Absent means "no restrictions",
    // which is the honest reading -- unlike the required sections above, where
    // absent would mean the file is broken.
    {
        const auto bytes = section(Section::TurnRestrictions);
        if (!bytes.empty())
        {
            if (bytes.size() % sizeof(TurnRestrictionRecord) != 0)
            {
                return malformed("turn restriction section is not a multiple of its record size");
            }
            mRestrictions = { reinterpret_cast<const TurnRestrictionRecord*>(bytes.data()),
                              bytes.size() / sizeof(TurnRestrictionRecord) };
        }
    }

    const auto strings = section(Section::Strings);
    mStrings = { reinterpret_cast<const char*>(strings.data()), strings.size() };

    if (mCsrOffsets.size() != mNodes.size() + 1)
    {
        return malformed("csr offsets has " + std::to_string(mCsrOffsets.size()) +
                         " entries for " + std::to_string(mNodes.size()) + " nodes");
    }

    // The A* heuristic's divisor. Taken from the header when the build wrote
    // one; scanned HERE, at open, for a graph predating the field. Never at
    // query time -- that scan is 320 MB on SoCal and it is the same answer
    // every time. Floored at 1 so it can be divided by.
    mMaxFreeFlowSpeedKph = mHeader->maxFreeFlowSpeedKph;
    if (mMaxFreeFlowSpeedKph == 0)
    {
        for (const SegmentRecord& segment : mSegments)
        {
            mMaxFreeFlowSpeedKph = std::max(mMaxFreeFlowSpeedKph,
                                            static_cast<std::uint32_t>(segment.freeFlowSpeedKph));
        }
    }
    mMaxFreeFlowSpeedKph = std::max(mMaxFreeFlowSpeedKph, 1U);

    // Rebuild the packed tree's level offsets. They are derivable from the
    // segment count and the fanout, so they are not stored -- one less thing
    // that can disagree with the data.
    mRTreeLevels.clear();
    std::uint32_t levelStart = 0;
    std::uint32_t levelCount = static_cast<std::uint32_t>(mSegments.size());
    while (true)
    {
        mRTreeLevels.push_back(levelStart);
        if (levelCount <= 1)
        {
            break;
        }
        levelStart += levelCount;
        levelCount = (levelCount + kRTreeFanout - 1) / kRTreeFanout;
    }

    return {};
}

std::string_view Graph::string(std::uint32_t offset) const
{
    if (offset == kNoString || offset >= mStrings.size())
    {
        return {};
    }
    const char* begin = mStrings.data() + offset;
    const std::size_t remaining = mStrings.size() - offset;
    const std::size_t length = ::strnlen(begin, remaining);
    return { begin, length };
}

std::optional<SegmentIndex> Graph::indexOf(SegmentId id) const
{
    const auto it = std::lower_bound(mIdIndex.begin(), mIdIndex.end(), id,
                                     [](const SegmentIdIndexEntry& entry, SegmentId value) {
                                         return entry.id < value;
                                     });
    if (it == mIdIndex.end() || it->id != id)
    {
        return std::nullopt;
    }
    return it->index;
}

std::ranges::iota_view<SegmentIndex, SegmentIndex> Graph::segmentsOfWay(std::int64_t wayId) const
{
    const auto it = std::lower_bound(mWayIndex.begin(), mWayIndex.end(), wayId,
                                     [](const WayIndexEntry& entry, std::int64_t value) {
                                         return entry.wayId < value;
                                     });
    if (it == mWayIndex.end() || it->wayId != wayId)
    {
        return std::views::iota(SegmentIndex { 0 }, SegmentIndex { 0 });
    }

    // The way index stores a contiguous RANGE because map_build emits a way's
    // segments together and in order. That is what makes "the piece of this way
    // that touches this junction" answerable at stage 4 -- and it is why there
    // is nothing here to materialise.
    return std::views::iota(it->firstSegment,
                            static_cast<SegmentIndex>(it->firstSegment + it->segmentCount));
}

bool Graph::turnAllowed(SegmentIndex fromSegment, NodeIndex viaNode, SegmentIndex toSegment) const
{
    if (mRestrictions.empty())
    {
        return true;
    }

    // The restrictions that apply to leaving `fromSegment` at `viaNode`. Sorted
    // by (from, via, to), so this is one binary search and then a short scan --
    // a junction has a handful of restrictions at most.
    const auto begin = std::lower_bound(
        mRestrictions.begin(), mRestrictions.end(), std::pair<SegmentIndex, NodeIndex> { fromSegment, viaNode },
        [](const TurnRestrictionRecord& record, const std::pair<SegmentIndex, NodeIndex>& key) {
            if (record.fromSegment != key.first)
            {
                return record.fromSegment < key.first;
            }
            return record.viaNode < key.second;
        });

    bool sawOnly = false;
    for (auto it = begin; it != mRestrictions.end(); ++it)
    {
        if (it->fromSegment != fromSegment || it->viaNode != viaNode)
        {
            break;
        }
        if (it->kind == kRestrictionProhibited)
        {
            if (it->toSegment == toSegment)
            {
                return false;
            }
        }
        else
        {
            // An `only_*` restriction bans every OTHER turn at this junction,
            // so a match permits and a non-match forbids -- the opposite sense
            // from a prohibition, and the reason both kinds live in one table
            // rather than one being expressed as the other.
            if (it->toSegment == toSegment)
            {
                return true;
            }
            sawOnly = true;
        }
    }

    return !sawOnly;
}

void Graph::queryBox(Coord west, Coord south, Coord east, Coord north,
                     const std::function<void(SegmentIndex)>& visit) const
{
    if (mRTree.empty() || mRTreeLevels.empty())
    {
        return;
    }

    const auto intersects = [&](const RTreeNode& node) {
        return !(node.east < west || node.west > east || node.north < south || node.south > north);
    };

    // Walk down from the root. Levels are stored bottom-up, so the root is the
    // single node at the last level.
    struct Frame
    {
        std::size_t level;
        std::uint32_t index;
    };

    std::vector<Frame> stack;
    stack.push_back({ mRTreeLevels.size() - 1, 0 });

    while (!stack.empty())
    {
        const Frame frame = stack.back();
        stack.pop_back();

        const std::uint32_t base = mRTreeLevels[frame.level];
        const std::uint32_t at = base + frame.index;
        if (at >= mRTree.size())
        {
            continue;
        }

        const RTreeNode& node = mRTree[at];
        if (!intersects(node))
        {
            continue;
        }

        if (frame.level == 0)
        {
            visit(node.payload);
            continue;
        }

        const std::size_t childLevel = frame.level - 1;
        const std::uint32_t childBase = mRTreeLevels[childLevel];
        const std::uint32_t childCount =
            (childLevel + 1 < mRTreeLevels.size())
                ? mRTreeLevels[childLevel + 1] - childBase
                : static_cast<std::uint32_t>(mRTree.size()) - childBase;

        for (std::uint32_t i = 0; i < kRTreeFanout; ++i)
        {
            const std::uint32_t child = node.payload + i;
            if (child >= childCount)
            {
                break;
            }
            stack.push_back({ childLevel, child });
        }
    }
}

std::vector<Match> Graph::nearest(Coord lat, Coord lon, double radiusM, std::size_t maxCandidates,
                                  std::optional<double> headingDeg) const
{
    std::vector<Match> out;
    if (radiusM <= 0.0 || maxCandidates == 0)
    {
        return out;
    }

    // Degrees per metre at this latitude, so the box is square on the ground
    // rather than square in degrees. At 34 degrees north that is a 20% error in
    // longitude if ignored, which quietly narrows the search east-west.
    const double rad = std::numbers::pi / 180.0;
    const double degPerM = 1.0 / (kEarthRadiusM * rad);
    const double cosLat = std::max(0.01, std::cos(toDegrees(lat) * rad));

    const auto dLat = static_cast<Coord>(radiusM * degPerM * 1e7);
    const auto dLon = static_cast<Coord>(radiusM * degPerM / cosLat * 1e7);

    struct Scored
    {
        Match match;
        double score;
    };
    std::vector<Scored> scored;

    queryBox(lon - dLon, lat - dLat, lon + dLon, lat + dLat, [&](SegmentIndex index) {
        if (index >= mSegments.size())
        {
            return;
        }
        const SegmentRecord& segment = mSegments[index];
        if (segment.routeClass == 0)
        {
            // Not routable at all -- a river, a building outline. The spatial
            // index carries everything; the filter is here.
            return;
        }

        const auto geometry = geometryOf(segment);
        if (geometry.size() < 4)
        {
            return;
        }

        double best = std::numeric_limits<double>::max();
        Projection bestProjection;
        std::size_t bestLeg = 0;
        double runningCm = 0.0;
        double bestOffsetCm = 0.0;

        for (std::size_t i = 0; i + 3 < geometry.size(); i += 2)
        {
            const Coord aLat = geometry[i];
            const Coord aLon = geometry[i + 1];
            const Coord bLat = geometry[i + 2];
            const Coord bLon = geometry[i + 3];

            const Projection projection = projectOnto(lat, lon, aLat, aLon, bLat, bLon);
            const double legM = distanceM(aLat, aLon, bLat, bLon);

            if (projection.distanceM < best)
            {
                best = projection.distanceM;
                bestProjection = projection;
                bestLeg = i;
                bestOffsetCm = runningCm + projection.t * legM * 100.0;
            }
            runningCm += legM * 100.0;
        }

        if (best > radiusM)
        {
            return;
        }

        Match match;
        match.segment = index;
        match.distanceM = best;
        match.offsetCm = static_cast<std::uint32_t>(bestOffsetCm);
        match.lat = bestProjection.lat;
        match.lon = bestProjection.lon;
        match.bearingDeg = bearingDeg(geometry[bestLeg], geometry[bestLeg + 1],
                                      geometry[bestLeg + 2], geometry[bestLeg + 3]);

        // Distance alone picks the frontage road as often as the freeway. When
        // a heading is available, disagreement with the road's bearing is worth
        // roughly as much as tens of metres of distance -- and a road may be
        // travelled either way, so the penalty is against the better of the two.
        double score = best + classPenaltyM(segment.routeClass);
        if (headingDeg.has_value())
        {
            const double forward = bearingDeltaDeg(*headingDeg, match.bearingDeg);
            const double backward = bearingDeltaDeg(*headingDeg, match.bearingDeg + 180.0);
            score += std::min(forward, backward) * 0.5;

            // Driving the wrong way down a one-way road is not a candidate.
            //
            // THIS is what separates the two carriageways of a divided highway.
            // They are parallel, metres apart and the same class, so distance
            // cannot choose and the heading agrees with one of them equally
            // well in reverse -- a road may be driven either way, so the term
            // above is deliberately direction-agnostic. Oneway is the only
            // thing left, and without it a matcher puts a northbound vehicle on
            // the southbound carriageway roughly half the time.
            const bool travellingForward = forward <= backward;
            const bool blocked = travellingForward
                                     ? (segment.flags & kFlagOnewayBackward) != 0
                                     : (segment.flags & kFlagOnewayForward) != 0;
            if (blocked)
            {
                // Penalised rather than dropped: OSM oneway tagging is wrong
                // often enough that refusing outright would lose the road
                // entirely, and a driver on a road we think is one-way the
                // other way still wants its name.
                score += 500.0;
            }
        }

        scored.push_back({ match, score });
    });

    std::sort(scored.begin(), scored.end(),
              [](const Scored& a, const Scored& b) { return a.score < b.score; });

    out.reserve(std::min(scored.size(), maxCandidates));
    for (std::size_t i = 0; i < scored.size() && i < maxCandidates; ++i)
    {
        out.push_back(scored[i].match);
    }
    return out;
}

} // namespace road_graph
