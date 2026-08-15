// SPDX-License-Identifier: GPL-3.0-or-later
#include "road_graph/builder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace road_graph
{
namespace
{

std::uint16_t flagsOf(const map_rules::RoadClassification& classification)
{
    std::uint16_t flags = 0;
    if (classification.onewayForward)
    {
        flags |= kFlagOnewayForward;
    }
    if (classification.onewayBackward)
    {
        flags |= kFlagOnewayBackward;
    }
    if (classification.hasPosted)
    {
        flags |= kFlagHasPosted;
    }
    if (classification.isBridge)
    {
        flags |= kFlagBridge;
    }
    if (classification.isTunnel)
    {
        flags |= kFlagTunnel;
    }
    return flags;
}

std::uint32_t polylineLengthCm(const std::vector<Coord>& geometry)
{
    double total = 0.0;
    for (std::size_t i = 0; i + 3 < geometry.size(); i += 2)
    {
        total += distanceM(geometry[i], geometry[i + 1], geometry[i + 2], geometry[i + 3]);
    }
    const double cm = total * 100.0;
    // Clamped rather than wrapped: a segment longer than 42 949 km is a bug
    // upstream, and wrapping would give it a tiny cost that a router would love.
    return cm > 4'000'000'000.0 ? 4'000'000'000u : static_cast<std::uint32_t>(cm);
}

} // namespace

NodeIndex Builder::nodeFor(std::int64_t osmNodeId, Coord lat, Coord lon)
{
    const auto found = mNodeIds.find(osmNodeId);
    if (found != mNodeIds.end())
    {
        return found->second;
    }

    const auto index = static_cast<NodeIndex>(mNodes.size());
    mNodes.push_back(NodeRecord { lat, lon });
    mNodeIds.emplace(osmNodeId, index);
    return index;
}

std::uint32_t Builder::internString(const std::string& text)
{
    if (text.empty())
    {
        return kNoString;
    }

    const auto found = mStringOffsets.find(text);
    if (found != mStringOffsets.end())
    {
        return found->second;
    }

    // Deduplicated: "Main Street" appears thousands of times in a city, and the
    // blob is mmap'd, so every copy is a page that has to be faulted in.
    const auto offset = static_cast<std::uint32_t>(mStrings.size());
    mStrings.append(text);
    mStrings.push_back('\0');
    mStringOffsets.emplace(text, offset);
    return offset;
}

void Builder::add(SegmentInput&& segment)
{
    if (segment.geometry.size() < 4)
    {
        // Fewer than two points is not a line. Dropped rather than stored,
        // because everything downstream assumes at least one leg.
        return;
    }

    Pending pending;
    pending.lengthCm = polylineLengthCm(segment.geometry);
    pending.from = nodeFor(segment.fromNodeId, segment.geometry[0], segment.geometry[1]);
    pending.to = nodeFor(segment.toNodeId, segment.geometry[segment.geometry.size() - 2],
                         segment.geometry[segment.geometry.size() - 1]);
    pending.input = std::move(segment);
    mSegments.push_back(std::move(pending));
}

void Builder::addRestriction(const RestrictionInput& restriction)
{
    mRestrictions.push_back(restriction);
}

Result<void> Builder::write(const std::filesystem::path& path, std::int64_t builtAtUnixS)
{
    if (mSegments.empty())
    {
        return invalid_argument("nothing to write: no segments were added");
    }

    // ---- Hilbert ordering (decision 5) -----------------------------------
    //
    // Nodes and segments both. OSM ids are chronological and therefore
    // spatially random, so an id-ordered graph makes an A* expansion fault on
    // almost every node it touches. Reordering is free here and is worth two
    // orders of magnitude at query time on a continental file.
    std::vector<NodeIndex> nodeOrder(mNodes.size());
    std::iota(nodeOrder.begin(), nodeOrder.end(), 0u);
    std::sort(nodeOrder.begin(), nodeOrder.end(), [&](NodeIndex a, NodeIndex b) {
        return hilbertOf(mNodes[a].lat, mNodes[a].lon) < hilbertOf(mNodes[b].lat, mNodes[b].lon);
    });

    std::vector<NodeIndex> nodeRemap(mNodes.size());
    std::vector<NodeRecord> orderedNodes(mNodes.size());
    for (std::size_t i = 0; i < nodeOrder.size(); ++i)
    {
        nodeRemap[nodeOrder[i]] = static_cast<NodeIndex>(i);
        orderedNodes[i] = mNodes[nodeOrder[i]];
    }

    std::vector<std::uint32_t> segmentOrder(mSegments.size());
    std::iota(segmentOrder.begin(), segmentOrder.end(), 0u);
    std::sort(segmentOrder.begin(), segmentOrder.end(), [&](std::uint32_t a, std::uint32_t b) {
        const auto& ga = mSegments[a].input.geometry;
        const auto& gb = mSegments[b].input.geometry;
        return hilbertOf(ga[0], ga[1]) < hilbertOf(gb[0], gb[1]);
    });

    // ---- Records ---------------------------------------------------------
    std::vector<SegmentRecord> segments;
    std::vector<Coord> geometry;
    segments.reserve(mSegments.size());

    for (const std::uint32_t from : segmentOrder)
    {
        const Pending& pending = mSegments[from];
        const SegmentInput& input = pending.input;

        SegmentRecord record {};
        record.id = input.id;
        record.osmWayId = input.osmWayId;
        record.geometryOffset = static_cast<std::uint32_t>(geometry.size() / 2);
        record.geometryCount = static_cast<std::uint32_t>(input.geometry.size() / 2);
        record.fromNode = nodeRemap[pending.from];
        record.toNode = nodeRemap[pending.to];
        record.nameOffset = internString(input.name);
        record.refOffset = internString(input.ref);
        record.lengthCm = pending.lengthCm;
        record.postedSpeedKph = input.classification.postedSpeedKph;
        record.freeFlowSpeedKph = input.classification.freeFlowSpeedKph;
        record.accessMask = input.classification.access;
        record.flags = flagsOf(input.classification);
        record.renderClass = static_cast<std::uint8_t>(input.classification.renderClass);
        record.routeClass = static_cast<std::uint8_t>(input.classification.routeClass);
        record.postedSource = static_cast<std::uint8_t>(input.classification.postedSource);
        record.laneCount = input.classification.laneCount;
        record.layer = input.classification.layer;

        geometry.insert(geometry.end(), input.geometry.begin(), input.geometry.end());
        segments.push_back(record);
    }

    // ---- CSR -------------------------------------------------------------
    //
    // Built AFTER the reordering, so a node's neighbours are near it in the
    // file as well as in the world.
    std::vector<std::uint32_t> degree(orderedNodes.size() + 1, 0);
    const auto forwardOpen = [](const SegmentRecord& s) {
        return (s.flags & kFlagOnewayBackward) == 0;
    };
    const auto backwardOpen = [](const SegmentRecord& s) {
        return (s.flags & kFlagOnewayForward) == 0;
    };

    for (const SegmentRecord& segment : segments)
    {
        if (segment.routeClass == 0 || segment.accessMask == 0)
        {
            continue;
        }
        if (forwardOpen(segment))
        {
            ++degree[segment.fromNode];
        }
        if (backwardOpen(segment))
        {
            ++degree[segment.toNode];
        }
    }

    std::vector<std::uint32_t> offsets(orderedNodes.size() + 1, 0);
    std::uint32_t running = 0;
    for (std::size_t i = 0; i < orderedNodes.size(); ++i)
    {
        offsets[i] = running;
        running += degree[i];
    }
    offsets[orderedNodes.size()] = running;

    std::vector<EdgeRecord> edges(running);
    std::vector<std::uint32_t> cursor(offsets.begin(), offsets.end());

    for (std::uint32_t index = 0; index < segments.size(); ++index)
    {
        const SegmentRecord& segment = segments[index];
        if (segment.routeClass == 0 || segment.accessMask == 0)
        {
            continue;
        }

        // Free-flow traversal time. Deciseconds so a 40 km/h residential
        // segment of 30 m is 27 rather than 2, which matters when a route is a
        // few hundred short segments and the rounding compounds.
        const double speedKph = segment.freeFlowSpeedKph > 0 ? segment.freeFlowSpeedKph : 5;
        const double seconds = (static_cast<double>(segment.lengthCm) / 100.0) / (speedKph / 3.6);
        const auto costDs = static_cast<std::uint32_t>(std::max(1.0, seconds * 10.0));

        if (forwardOpen(segment))
        {
            EdgeRecord edge {};
            edge.segment = index;
            edge.target = segment.toNode;
            edge.costDs = costDs;
            edge.forward = 1;
            edges[cursor[segment.fromNode]++] = edge;
        }
        if (backwardOpen(segment))
        {
            EdgeRecord edge {};
            edge.segment = index;
            edge.target = segment.fromNode;
            edge.costDs = costDs;
            edge.forward = 0;
            edges[cursor[segment.toNode]++] = edge;
        }
    }

    // ---- Indices ---------------------------------------------------------
    std::vector<SegmentIdIndexEntry> idIndex;
    idIndex.reserve(segments.size());
    for (std::uint32_t i = 0; i < segments.size(); ++i)
    {
        idIndex.push_back(SegmentIdIndexEntry { segments[i].id, i, 0 });
    }
    std::sort(idIndex.begin(), idIndex.end(),
              [](const SegmentIdIndexEntry& a, const SegmentIdIndexEntry& b) {
                  return a.id < b.id;
              });

    // The way index needs each way's segments CONTIGUOUS and in order, which
    // the Hilbert sort has just destroyed. Rebuilt by sorting a copy: the
    // entries name a range, so the range has to exist.
    //
    // Segments are therefore emitted Hilbert-ordered, and the way index maps a
    // way to the sorted-by-(way, ordinal) positions -- which is why this walks
    // a second ordering rather than the one in `segments`.
    std::vector<std::uint32_t> byWay(segments.size());
    std::iota(byWay.begin(), byWay.end(), 0u);
    std::sort(byWay.begin(), byWay.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (segments[a].osmWayId != segments[b].osmWayId)
        {
            return segments[a].osmWayId < segments[b].osmWayId;
        }
        return ordinalOf(segments[a].id) < ordinalOf(segments[b].id);
    });

    // Re-emit segments in way order so a way's pieces really are contiguous,
    // then translate every index that referred to the old order. Locality is
    // preserved because a way is a short run of adjacent geometry anyway.
    std::vector<std::uint32_t> segmentRemap(segments.size());
    std::vector<SegmentRecord> wayOrdered(segments.size());
    for (std::uint32_t i = 0; i < byWay.size(); ++i)
    {
        segmentRemap[byWay[i]] = i;
        wayOrdered[i] = segments[byWay[i]];
    }
    segments.swap(wayOrdered);

    for (EdgeRecord& edge : edges)
    {
        edge.segment = segmentRemap[edge.segment];
    }
    for (SegmentIdIndexEntry& entry : idIndex)
    {
        entry.index = segmentRemap[entry.index];
    }

    std::vector<WayIndexEntry> wayIndex;
    for (std::uint32_t i = 0; i < segments.size(); ++i)
    {
        if (!wayIndex.empty() && wayIndex.back().wayId == segments[i].osmWayId)
        {
            ++wayIndex.back().segmentCount;
            continue;
        }
        wayIndex.push_back(WayIndexEntry { segments[i].osmWayId, i, 1 });
    }

    // ---- Turn restrictions -----------------------------------------------
    //
    // Resolved HERE, against the finished layout, because a restriction names
    // ways and the graph is made of segments. Finding "the piece of this way
    // that touches this junction" needs the way index, which is exactly why
    // that index is in the artifact (decision 3) rather than being dropped as
    // build scratch.
    std::vector<TurnRestrictionRecord> restrictions;
    {
        const auto segmentsOf = [&](std::int64_t wayId) -> std::pair<std::uint32_t, std::uint32_t> {
            const auto it = std::lower_bound(wayIndex.begin(), wayIndex.end(), wayId,
                                             [](const WayIndexEntry& entry, std::int64_t value) {
                                                 return entry.wayId < value;
                                             });
            if (it == wayIndex.end() || it->wayId != wayId)
            {
                return { 0, 0 };
            }
            return { it->firstSegment, it->segmentCount };
        };

        for (const RestrictionInput& input : mRestrictions)
        {
            ++mRestrictionCounts.offered;

            const auto viaIt = mNodeIds.find(input.viaNodeId);
            if (viaIt == mNodeIds.end())
            {
                ++mRestrictionCounts.unresolved;
                continue;
            }
            const NodeIndex via = nodeRemap[viaIt->second];

            // The piece of each way that actually touches the junction. A way
            // split into six segments contributes exactly one of them here, and
            // restricting the wrong one would ban a turn somewhere else on the
            // same road.
            const auto findTouching = [&](std::int64_t wayId) -> SegmentIndex {
                const auto [first, count] = segmentsOf(wayId);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const SegmentRecord& candidate = segments[first + i];
                    if (candidate.fromNode == via || candidate.toNode == via)
                    {
                        return first + i;
                    }
                }
                return kNoSegment;
            };

            const SegmentIndex from = findTouching(input.fromWayId);
            const SegmentIndex to = findTouching(input.toWayId);
            if (from == kNoSegment || to == kNoSegment)
            {
                // A way that contributed nothing routable -- a footpath, a
                // private drive -- or a via node that is not a junction in our
                // graph. Common enough on a real extract to be counted rather
                // than logged.
                ++mRestrictionCounts.unresolved;
                continue;
            }

            TurnRestrictionRecord record {};
            record.fromSegment = from;
            record.viaNode = via;
            record.toSegment = to;
            record.kind = input.only ? kRestrictionOnly : kRestrictionProhibited;
            restrictions.push_back(record);
            ++mRestrictionCounts.resolved;
        }

        // Sorted so a lookup during search is a binary search on (from, via),
        // which is what makes consulting this per transition affordable.
        std::sort(restrictions.begin(), restrictions.end(),
                  [](const TurnRestrictionRecord& a, const TurnRestrictionRecord& b) {
                      if (a.fromSegment != b.fromSegment)
                      {
                          return a.fromSegment < b.fromSegment;
                      }
                      if (a.viaNode != b.viaNode)
                      {
                          return a.viaNode < b.viaNode;
                      }
                      return a.toSegment < b.toSegment;
                  });
    }

    // ---- Packed R-tree ---------------------------------------------------
    //
    // Bottom-up over the segments in file order. No comparisons and no
    // balancing: the order already has locality, so grouping consecutive runs
    // gives tight boxes for free.
    std::vector<RTreeNode> tree;
    std::vector<std::uint32_t> levelStarts;

    {
        levelStarts.push_back(0);
        for (std::uint32_t i = 0; i < segments.size(); ++i)
        {
            const SegmentRecord& segment = segments[i];
            RTreeNode leaf {};
            leaf.west = std::numeric_limits<Coord>::max();
            leaf.south = std::numeric_limits<Coord>::max();
            leaf.east = std::numeric_limits<Coord>::min();
            leaf.north = std::numeric_limits<Coord>::min();
            for (std::uint32_t g = 0; g < segment.geometryCount; ++g)
            {
                const Coord lat = geometry[(segment.geometryOffset + g) * 2];
                const Coord lon = geometry[(segment.geometryOffset + g) * 2 + 1];
                leaf.west = std::min(leaf.west, lon);
                leaf.east = std::max(leaf.east, lon);
                leaf.south = std::min(leaf.south, lat);
                leaf.north = std::max(leaf.north, lat);
            }
            leaf.payload = i;
            tree.push_back(leaf);
        }

        std::uint32_t levelStart = 0;
        std::uint32_t levelCount = static_cast<std::uint32_t>(segments.size());
        while (levelCount > 1)
        {
            const std::uint32_t nextStart = levelStart + levelCount;
            const std::uint32_t nextCount = (levelCount + kRTreeFanout - 1) / kRTreeFanout;
            levelStarts.push_back(nextStart);

            for (std::uint32_t i = 0; i < nextCount; ++i)
            {
                RTreeNode parent {};
                parent.west = std::numeric_limits<Coord>::max();
                parent.south = std::numeric_limits<Coord>::max();
                parent.east = std::numeric_limits<Coord>::min();
                parent.north = std::numeric_limits<Coord>::min();
                parent.payload = i * kRTreeFanout;

                for (std::uint32_t c = 0; c < kRTreeFanout; ++c)
                {
                    const std::uint32_t child = i * kRTreeFanout + c;
                    if (child >= levelCount)
                    {
                        break;
                    }
                    const RTreeNode& node = tree[levelStart + child];
                    parent.west = std::min(parent.west, node.west);
                    parent.east = std::max(parent.east, node.east);
                    parent.south = std::min(parent.south, node.south);
                    parent.north = std::max(parent.north, node.north);
                }
                tree.push_back(parent);
            }

            levelStart = nextStart;
            levelCount = nextCount;
        }
    }

    // ---- Header and sections ---------------------------------------------
    FileHeader header {};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kFormatVersion;
    header.nodeCount = static_cast<std::uint32_t>(orderedNodes.size());
    header.segmentCount = static_cast<std::uint32_t>(segments.size());
    header.edgeCount = static_cast<std::uint32_t>(edges.size());
    header.geometryCount = static_cast<std::uint32_t>(geometry.size() / 2);
    header.builtAtUnixS = builtAtUnixS;

    header.west = std::numeric_limits<Coord>::max();
    header.south = std::numeric_limits<Coord>::max();
    header.east = std::numeric_limits<Coord>::min();
    header.north = std::numeric_limits<Coord>::min();
    for (std::size_t i = 0; i + 1 < geometry.size(); i += 2)
    {
        header.south = std::min(header.south, geometry[i]);
        header.north = std::max(header.north, geometry[i]);
        header.west = std::min(header.west, geometry[i + 1]);
        header.east = std::max(header.east, geometry[i + 1]);
    }

    struct Blob
    {
        Section kind;
        std::uint32_t elementSize;
        const void* data;
        std::uint64_t bytes;
    };

    const std::vector<Blob> blobs {
        { Section::Nodes, sizeof(NodeRecord), orderedNodes.data(),
          orderedNodes.size() * sizeof(NodeRecord) },
        { Section::Segments, sizeof(SegmentRecord), segments.data(),
          segments.size() * sizeof(SegmentRecord) },
        { Section::Geometry, sizeof(Coord), geometry.data(), geometry.size() * sizeof(Coord) },
        { Section::Strings, 1, mStrings.data(), mStrings.size() },
        { Section::CsrOffsets, sizeof(std::uint32_t), offsets.data(),
          offsets.size() * sizeof(std::uint32_t) },
        { Section::CsrEdges, sizeof(EdgeRecord), edges.data(), edges.size() * sizeof(EdgeRecord) },
        { Section::WayIndex, sizeof(WayIndexEntry), wayIndex.data(),
          wayIndex.size() * sizeof(WayIndexEntry) },
        { Section::SegmentIdIndex, sizeof(SegmentIdIndexEntry), idIndex.data(),
          idIndex.size() * sizeof(SegmentIdIndexEntry) },
        { Section::SpatialIndex, sizeof(RTreeNode), tree.data(), tree.size() * sizeof(RTreeNode) },
        { Section::TurnRestrictions, sizeof(TurnRestrictionRecord), restrictions.data(),
          restrictions.size() * sizeof(TurnRestrictionRecord) },
    };

    header.sectionCount = static_cast<std::uint32_t>(blobs.size());

    std::vector<SectionEntry> table(blobs.size());
    std::uint64_t at = sizeof(FileHeader) + sizeof(SectionEntry) * blobs.size();
    for (std::size_t i = 0; i < blobs.size(); ++i)
    {
        table[i].kind = static_cast<std::uint32_t>(blobs[i].kind);
        table[i].elementSize = blobs[i].elementSize;
        table[i].offset = at;
        table[i].length = blobs[i].bytes;
        at += blobs[i].bytes;
    }

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        return not_readable("cannot write " + path.string());
    }

    const auto put = [&](const void* data, std::size_t bytes) {
        return bytes == 0 || std::fwrite(data, 1, bytes, file) == bytes;
    };

    bool ok = put(&header, sizeof(header)) && put(table.data(), table.size() * sizeof(SectionEntry));
    for (const Blob& blob : blobs)
    {
        ok = ok && put(blob.data, static_cast<std::size_t>(blob.bytes));
    }
    std::fclose(file);

    if (!ok)
    {
        return not_readable("short write to " + path.string());
    }
    return {};
}

} // namespace road_graph
