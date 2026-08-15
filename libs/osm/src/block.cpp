// SPDX-License-Identifier: GPL-3.0-or-later
#include "osm/block.h"

#include <array>

#include "protowire/reader.h"

namespace osm
{
namespace
{

// HeaderBlock
constexpr std::uint32_t kHeaderBbox = 1;
constexpr std::uint32_t kHeaderRequiredFeatures = 4;
constexpr std::uint32_t kHeaderOptionalFeatures = 5;
constexpr std::uint32_t kHeaderWritingProgram = 16;
constexpr std::uint32_t kHeaderReplicationTimestamp = 32;

// HeaderBBox, in nanodegrees.
constexpr std::uint32_t kBboxLeft = 1;
constexpr std::uint32_t kBboxRight = 2;
constexpr std::uint32_t kBboxTop = 3;
constexpr std::uint32_t kBboxBottom = 4;

// PrimitiveBlock
constexpr std::uint32_t kBlockStringTable = 1;
constexpr std::uint32_t kBlockPrimitiveGroup = 2;
constexpr std::uint32_t kBlockGranularity = 17;
constexpr std::uint32_t kBlockLatOffset = 19;
constexpr std::uint32_t kBlockLonOffset = 20;

// PrimitiveGroup
constexpr std::uint32_t kGroupNodes = 1;
constexpr std::uint32_t kGroupDense = 2;
constexpr std::uint32_t kGroupWays = 3;
constexpr std::uint32_t kGroupRelations = 4;

// StringTable
constexpr std::uint32_t kStringTableEntry = 1;

// Node / Way / Relation share field numbers for the common parts.
constexpr std::uint32_t kEntityId = 1;
constexpr std::uint32_t kEntityKeys = 2;
constexpr std::uint32_t kEntityVals = 3;
constexpr std::uint32_t kNodeLat = 8;
constexpr std::uint32_t kNodeLon = 9;
constexpr std::uint32_t kWayRefs = 8;
constexpr std::uint32_t kRelationRoles = 8;
constexpr std::uint32_t kRelationMemids = 9;
constexpr std::uint32_t kRelationTypes = 10;

// DenseNodes
constexpr std::uint32_t kDenseId = 1;
constexpr std::uint32_t kDenseLat = 8;
constexpr std::uint32_t kDenseLon = 9;
constexpr std::uint32_t kDenseKeysVals = 10;

// The spec's default. A file that does not say uses 100 nanodegrees per unit,
// which is exactly the 1e-7 degrees this tree stores -- so the common case is a
// divide by 100 and nothing else.
constexpr std::int64_t kDefaultGranularity = 100;

// One block's coordinate frame.
struct Frame
{
    std::int64_t granularity { kDefaultGranularity };
    std::int64_t latOffset { 0 };
    std::int64_t lonOffset { 0 };

    // nanodegrees -> 1e-7 degrees, rounded rather than truncated.
    //
    // Getting granularity wrong here does not fail: it puts every coordinate in
    // the file off by whatever factor, which renders as a map of the right
    // shape in the wrong place.
    Coord toCoord(std::int64_t delta, std::int64_t offset) const
    {
        const std::int64_t nano = offset + granularity * delta;
        const std::int64_t rounded = nano >= 0 ? (nano + 50) / 100 : (nano - 50) / 100;
        return static_cast<Coord>(rounded);
    }

    Coord lat(std::int64_t delta) const { return toCoord(delta, latOffset); }
    Coord lon(std::int64_t delta) const { return toCoord(delta, lonOffset); }
};

// Read a packed repeated field into `out` using `read` per element.
template <typename T, typename ReadFn>
Result<void> readPacked(protowire::Reader& reader, std::vector<T>& out, ReadFn read,
                        std::size_t blockOffset)
{
    auto sub = reader.sub();
    if (!sub)
    {
        return from_wire(sub.error(), blockOffset);
    }

    while (!sub->done())
    {
        auto value = read(*sub);
        if (!value)
        {
            return from_wire(value.error(), blockOffset);
        }
        out.push_back(static_cast<T>(*value));
    }
    return {};
}

Result<void> decodeStringTable(protowire::Reader& reader, std::vector<std::string>& out,
                               std::size_t blockOffset)
{
    auto table = reader.sub();
    if (!table)
    {
        return from_wire(table.error(), blockOffset);
    }

    while (!table->done())
    {
        auto field = table->field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        if (field->number == kStringTableEntry &&
            field->wire == protowire::WireType::LengthDelimited)
        {
            auto text = table->text();
            if (!text)
            {
                return from_wire(text.error(), blockOffset);
            }
            out.emplace_back(*text);
        }
        else if (auto skipped = table->skip(field->wire); !skipped)
        {
            return from_wire(skipped.error(), blockOffset);
        }
    }
    return {};
}

// Attach a run of (key, value) index pairs, checking them against the table.
//
// An index past the end of the string table is refused rather than clamped: it
// would otherwise silently give an entity a tag belonging to something else.
Result<void> appendTags(Block& block, std::span<const std::uint32_t> keys,
                        std::span<const std::uint32_t> vals, std::size_t blockOffset)
{
    if (keys.size() != vals.size())
    {
        return malformed("entity with " + std::to_string(keys.size()) + " tag keys and " +
                             std::to_string(vals.size()) + " values",
                         blockOffset);
    }

    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i] >= block.mStrings.size() || vals[i] >= block.mStrings.size())
        {
            return malformed("tag index past the end of the string table", blockOffset);
        }
        block.mTags.push_back(Tag { keys[i], vals[i] });
    }
    return {};
}

Result<void> decodeDenseNodes(protowire::Reader& reader, Block& block, const Frame& frame,
                              std::size_t blockOffset)
{
    auto dense = reader.sub();
    if (!dense)
    {
        return from_wire(dense.error(), blockOffset);
    }

    std::vector<std::int64_t> ids;
    std::vector<std::int64_t> lats;
    std::vector<std::int64_t> lons;
    std::vector<std::int32_t> keysVals;

    while (!dense->done())
    {
        auto field = dense->field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        const auto zigzag = [](protowire::Reader& r) { return r.zigzag(); };
        const auto int32 = [](protowire::Reader& r) { return r.int32(); };

        switch (field->number)
        {
            case kDenseId:
                if (auto ok = readPacked(*dense, ids, zigzag, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kDenseLat:
                if (auto ok = readPacked(*dense, lats, zigzag, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kDenseLon:
                if (auto ok = readPacked(*dense, lons, zigzag, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kDenseKeysVals:
                if (auto ok = readPacked(*dense, keysVals, int32, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            default:
                if (auto skipped = dense->skip(field->wire); !skipped)
                {
                    return from_wire(skipped.error(), blockOffset);
                }
                break;
        }
    }

    if (ids.size() != lats.size() || ids.size() != lons.size())
    {
        return malformed("dense nodes with " + std::to_string(ids.size()) + " ids, " +
                             std::to_string(lats.size()) + " lats and " +
                             std::to_string(lons.size()) + " lons",
                         blockOffset);
    }

    // keys_vals IS ONE FLAT ARRAY, ZERO-TERMINATED PER NODE -- not a list per
    // node. A node with no tags contributes a single 0, and reading it as
    // anything else shifts every later node's tags by one, silently, so that
    // every node in the block ends up with its neighbour's name. When the field
    // is absent entirely, no node in the block has tags.
    std::size_t kv = 0;

    // Deltas accumulate ACROSS THE GROUP and reset with it. Carrying them into
    // the next group puts the rest of the file in the ocean.
    std::int64_t id = 0;
    std::int64_t lat = 0;
    std::int64_t lon = 0;

    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        id += ids[i];
        lat += lats[i];
        lon += lons[i];

        Node node;
        node.id = id;
        node.lat = frame.lat(lat);
        node.lon = frame.lon(lon);
        node.tagBegin = static_cast<std::uint32_t>(block.mTags.size());

        if (!keysVals.empty())
        {
            while (kv < keysVals.size() && keysVals[kv] != 0)
            {
                if (kv + 1 >= keysVals.size())
                {
                    return malformed("dense keys_vals ends mid-pair", blockOffset);
                }
                const auto key = static_cast<std::uint32_t>(keysVals[kv]);
                const auto value = static_cast<std::uint32_t>(keysVals[kv + 1]);
                if (key >= block.mStrings.size() || value >= block.mStrings.size())
                {
                    return malformed("dense tag index past the end of the string table",
                                     blockOffset);
                }
                block.mTags.push_back(Tag { key, value });
                kv += 2;
            }
            // Step over the terminator.
            ++kv;
        }

        node.tagCount = static_cast<std::uint32_t>(block.mTags.size()) - node.tagBegin;
        block.mNodes.push_back(node);
    }

    return {};
}

Result<void> decodePlainNode(protowire::Reader& reader, Block& block, const Frame& frame,
                             std::size_t blockOffset)
{
    auto node = reader.sub();
    if (!node)
    {
        return from_wire(node.error(), blockOffset);
    }

    std::vector<std::uint32_t> keys;
    std::vector<std::uint32_t> vals;
    std::int64_t id = 0;
    std::int64_t lat = 0;
    std::int64_t lon = 0;

    while (!node->done())
    {
        auto field = node->field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        const auto varint = [](protowire::Reader& r) { return r.varint(); };

        switch (field->number)
        {
            case kEntityId:
            {
                auto value = node->zigzag();
                if (!value)
                {
                    return from_wire(value.error(), blockOffset);
                }
                id = *value;
                break;
            }
            case kEntityKeys:
                if (auto ok = readPacked(*node, keys, varint, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kEntityVals:
                if (auto ok = readPacked(*node, vals, varint, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kNodeLat:
            {
                auto value = node->zigzag();
                if (!value)
                {
                    return from_wire(value.error(), blockOffset);
                }
                lat = *value;
                break;
            }
            case kNodeLon:
            {
                auto value = node->zigzag();
                if (!value)
                {
                    return from_wire(value.error(), blockOffset);
                }
                lon = *value;
                break;
            }
            default:
                if (auto skipped = node->skip(field->wire); !skipped)
                {
                    return from_wire(skipped.error(), blockOffset);
                }
                break;
        }
    }

    Node out;
    out.id = id;
    // A plain Node carries absolute values, not deltas -- the only place in the
    // format where a coordinate is not accumulated.
    out.lat = frame.lat(lat);
    out.lon = frame.lon(lon);
    out.tagBegin = static_cast<std::uint32_t>(block.mTags.size());
    if (auto ok = appendTags(block, keys, vals, blockOffset); !ok)
    {
        return ok;
    }
    out.tagCount = static_cast<std::uint32_t>(block.mTags.size()) - out.tagBegin;
    block.mNodes.push_back(out);
    return {};
}

Result<void> decodeWay(protowire::Reader& reader, Block& block, std::size_t blockOffset)
{
    auto way = reader.sub();
    if (!way)
    {
        return from_wire(way.error(), blockOffset);
    }

    std::vector<std::uint32_t> keys;
    std::vector<std::uint32_t> vals;
    std::vector<std::int64_t> deltas;
    std::int64_t id = 0;

    while (!way->done())
    {
        auto field = way->field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        const auto varint = [](protowire::Reader& r) { return r.varint(); };
        const auto zigzag = [](protowire::Reader& r) { return r.zigzag(); };

        switch (field->number)
        {
            case kEntityId:
            {
                // A way id is int64, NOT zigzag -- unlike a node id in
                // DenseNodes and unlike its own refs. Reading it as zigzag
                // halves every way id, which resolves to a different way.
                auto value = way->int64();
                if (!value)
                {
                    return from_wire(value.error(), blockOffset);
                }
                id = *value;
                break;
            }
            case kEntityKeys:
                if (auto ok = readPacked(*way, keys, varint, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kEntityVals:
                if (auto ok = readPacked(*way, vals, varint, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kWayRefs:
                if (auto ok = readPacked(*way, deltas, zigzag, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            default:
                if (auto skipped = way->skip(field->wire); !skipped)
                {
                    return from_wire(skipped.error(), blockOffset);
                }
                break;
        }
    }

    Way out;
    out.id = id;
    out.refBegin = static_cast<std::uint32_t>(block.mRefs.size());

    // Refs are DELTA-CODED. Reading them as absolute makes every way a line
    // from node 1 to node 2 to node 3 -- which is a real, drawable, entirely
    // wrong shape.
    std::int64_t ref = 0;
    for (const std::int64_t delta : deltas)
    {
        ref += delta;
        block.mRefs.push_back(ref);
    }
    out.refCount = static_cast<std::uint32_t>(block.mRefs.size()) - out.refBegin;

    out.tagBegin = static_cast<std::uint32_t>(block.mTags.size());
    if (auto ok = appendTags(block, keys, vals, blockOffset); !ok)
    {
        return ok;
    }
    out.tagCount = static_cast<std::uint32_t>(block.mTags.size()) - out.tagBegin;

    block.mWays.push_back(out);
    return {};
}

Result<void> decodeRelation(protowire::Reader& reader, Block& block, std::size_t blockOffset)
{
    auto relation = reader.sub();
    if (!relation)
    {
        return from_wire(relation.error(), blockOffset);
    }

    std::vector<std::uint32_t> keys;
    std::vector<std::uint32_t> vals;
    std::vector<std::int32_t> roles;
    std::vector<std::int64_t> memidDeltas;
    std::vector<std::int32_t> types;
    std::int64_t id = 0;

    while (!relation->done())
    {
        auto field = relation->field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        const auto varint = [](protowire::Reader& r) { return r.varint(); };
        const auto zigzag = [](protowire::Reader& r) { return r.zigzag(); };
        const auto int32 = [](protowire::Reader& r) { return r.int32(); };

        switch (field->number)
        {
            case kEntityId:
            {
                auto value = relation->int64();
                if (!value)
                {
                    return from_wire(value.error(), blockOffset);
                }
                id = *value;
                break;
            }
            case kEntityKeys:
                if (auto ok = readPacked(*relation, keys, varint, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kEntityVals:
                if (auto ok = readPacked(*relation, vals, varint, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kRelationRoles:
                // Role indices are NOT delta-coded, unlike the member ids
                // beside them in the same message.
                if (auto ok = readPacked(*relation, roles, int32, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kRelationMemids:
                if (auto ok = readPacked(*relation, memidDeltas, zigzag, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kRelationTypes:
                if (auto ok = readPacked(*relation, types, int32, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            default:
                if (auto skipped = relation->skip(field->wire); !skipped)
                {
                    return from_wire(skipped.error(), blockOffset);
                }
                break;
        }
    }

    if (memidDeltas.size() != types.size() || memidDeltas.size() != roles.size())
    {
        return malformed("relation with " + std::to_string(memidDeltas.size()) + " members, " +
                             std::to_string(types.size()) + " types and " +
                             std::to_string(roles.size()) + " roles",
                         blockOffset);
    }

    Relation out;
    out.id = id;
    out.memberBegin = static_cast<std::uint32_t>(block.mMembers.size());

    std::int64_t memid = 0;
    for (std::size_t i = 0; i < memidDeltas.size(); ++i)
    {
        memid += memidDeltas[i];

        if (types[i] < 0 || types[i] > 2)
        {
            return malformed("relation member of type " + std::to_string(types[i]), blockOffset);
        }
        if (roles[i] < 0 || static_cast<std::size_t>(roles[i]) >= block.mStrings.size())
        {
            return malformed("relation role index past the end of the string table", blockOffset);
        }

        Member member;
        member.ref = memid;
        member.type = static_cast<MemberType>(types[i]);
        member.roleIndex = static_cast<std::uint32_t>(roles[i]);
        block.mMembers.push_back(member);
    }
    out.memberCount = static_cast<std::uint32_t>(block.mMembers.size()) - out.memberBegin;

    out.tagBegin = static_cast<std::uint32_t>(block.mTags.size());
    if (auto ok = appendTags(block, keys, vals, blockOffset); !ok)
    {
        return ok;
    }
    out.tagCount = static_cast<std::uint32_t>(block.mTags.size()) - out.tagBegin;

    block.mRelations.push_back(out);
    return {};
}

Result<void> decodeGroup(protowire::Reader& reader, Block& block, const Frame& frame,
                         std::size_t blockOffset)
{
    auto group = reader.sub();
    if (!group)
    {
        return from_wire(group.error(), blockOffset);
    }

    while (!group->done())
    {
        auto field = group->field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        switch (field->number)
        {
            case kGroupNodes:
                if (auto ok = decodePlainNode(*group, block, frame, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kGroupDense:
                if (auto ok = decodeDenseNodes(*group, block, frame, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kGroupWays:
                if (auto ok = decodeWay(*group, block, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            case kGroupRelations:
                if (auto ok = decodeRelation(*group, block, blockOffset); !ok)
                {
                    return ok;
                }
                break;
            default:
                // Changesets and anything added later.
                if (auto skipped = group->skip(field->wire); !skipped)
                {
                    return from_wire(skipped.error(), blockOffset);
                }
                break;
        }
    }

    return {};
}

} // namespace

Result<Header> decodeHeaderBlock(std::span<const std::uint8_t> bytes)
{
    Header header;
    protowire::Reader reader(bytes);

    while (!reader.done())
    {
        auto field = reader.field();
        if (!field)
        {
            return from_wire(field.error());
        }

        switch (field->number)
        {
            case kHeaderBbox:
            {
                auto bbox = reader.sub();
                if (!bbox)
                {
                    return from_wire(bbox.error());
                }
                header.hasBbox = true;
                while (!bbox->done())
                {
                    auto inner = bbox->field();
                    if (!inner)
                    {
                        return from_wire(inner.error());
                    }
                    if (inner->wire != protowire::WireType::Varint)
                    {
                        if (auto skipped = bbox->skip(inner->wire); !skipped)
                        {
                            return from_wire(skipped.error());
                        }
                        continue;
                    }
                    auto value = bbox->zigzag();
                    if (!value)
                    {
                        return from_wire(value.error());
                    }
                    // Nanodegrees, sint64.
                    const double degrees = static_cast<double>(*value) * 1e-9;
                    switch (inner->number)
                    {
                        case kBboxLeft:
                            header.west = degrees;
                            break;
                        case kBboxRight:
                            header.east = degrees;
                            break;
                        case kBboxTop:
                            header.north = degrees;
                            break;
                        case kBboxBottom:
                            header.south = degrees;
                            break;
                        default:
                            break;
                    }
                }
                break;
            }
            case kHeaderRequiredFeatures:
            {
                auto text = reader.text();
                if (!text)
                {
                    return from_wire(text.error());
                }
                header.requiredFeatures.emplace_back(*text);
                break;
            }
            case kHeaderOptionalFeatures:
            {
                auto text = reader.text();
                if (!text)
                {
                    return from_wire(text.error());
                }
                header.optionalFeatures.emplace_back(*text);
                break;
            }
            case kHeaderWritingProgram:
            {
                auto text = reader.text();
                if (!text)
                {
                    return from_wire(text.error());
                }
                header.writingProgram.assign(*text);
                break;
            }
            case kHeaderReplicationTimestamp:
            {
                auto value = reader.int64();
                if (!value)
                {
                    return from_wire(value.error());
                }
                header.replicationTimestamp = *value;
                break;
            }
            default:
                if (auto skipped = reader.skip(field->wire); !skipped)
                {
                    return from_wire(skipped.error());
                }
                break;
        }
    }

    return header;
}

Result<void> checkRequiredFeatures(const Header& header)
{
    // What this build actually implements. `DenseNodes` and the schema version
    // are the two every real file carries.
    static constexpr std::array<std::string_view, 2> kKnown {
        "OsmSchema-V0.6",
        "DenseNodes",
    };

    for (const std::string& feature : header.requiredFeatures)
    {
        // Called out by name because the consequence is specific: a full-history
        // file carries several versions of the same node id, and the node store
        // has exactly one slot per id -- so the last version silently wins and
        // some of the geometry is from whenever that edit happened.
        if (feature == "HistoricalInformation")
        {
            return unsupported(
                "this is a full-history PBF; the node store holds one version per id, "
                "so reading it would silently mix edits from different dates");
        }

        bool known = false;
        for (const std::string_view candidate : kKnown)
        {
            if (feature == candidate)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            // Refused rather than ignored. required_features exists precisely so
            // a reader that would misinterpret the file stops instead.
            return unsupported("file requires '" + feature + "', which this build does not implement");
        }
    }

    return {};
}

Result<Block> decodeDataBlock(std::span<const std::uint8_t> bytes, std::size_t blockOffset)
{
    Block block;
    Frame frame;

    // Two passes over the block's fields: the string table and the coordinate
    // frame have to be known before any group is decoded, and the format does
    // not promise they come first.
    protowire::Reader scan(bytes);
    while (!scan.done())
    {
        auto field = scan.field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        switch (field->number)
        {
            case kBlockStringTable:
                if (auto ok = decodeStringTable(scan, block.mStrings, blockOffset); !ok)
                {
                    return std::unexpected(ok.error());
                }
                break;
            case kBlockGranularity:
            {
                auto value = scan.int32();
                if (!value)
                {
                    return from_wire(value.error(), blockOffset);
                }
                if (*value <= 0)
                {
                    return malformed("granularity of " + std::to_string(*value), blockOffset);
                }
                frame.granularity = *value;
                break;
            }
            case kBlockLatOffset:
            {
                auto value = scan.int64();
                if (!value)
                {
                    return from_wire(value.error(), blockOffset);
                }
                frame.latOffset = *value;
                break;
            }
            case kBlockLonOffset:
            {
                auto value = scan.int64();
                if (!value)
                {
                    return from_wire(value.error(), blockOffset);
                }
                frame.lonOffset = *value;
                break;
            }
            default:
                if (auto skipped = scan.skip(field->wire); !skipped)
                {
                    return from_wire(skipped.error(), blockOffset);
                }
                break;
        }
    }

    protowire::Reader reader(bytes);
    while (!reader.done())
    {
        auto field = reader.field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        if (field->number == kBlockPrimitiveGroup)
        {
            if (auto ok = decodeGroup(reader, block, frame, blockOffset); !ok)
            {
                return std::unexpected(ok.error());
            }
        }
        else if (auto skipped = reader.skip(field->wire); !skipped)
        {
            return from_wire(skipped.error(), blockOffset);
        }
    }

    return block;
}

Result<BlockContents> peekDataBlock(std::span<const std::uint8_t> bytes, std::size_t blockOffset)
{
    BlockContents contents;

    protowire::Reader reader(bytes);
    while (!reader.done())
    {
        auto field = reader.field();
        if (!field)
        {
            return from_wire(field.error(), blockOffset);
        }

        if (field->number != kBlockPrimitiveGroup)
        {
            if (auto skipped = reader.skip(field->wire); !skipped)
            {
                return from_wire(skipped.error(), blockOffset);
            }
            continue;
        }

        auto group = reader.sub();
        if (!group)
        {
            return from_wire(group.error(), blockOffset);
        }

        while (!group->done())
        {
            auto inner = group->field();
            if (!inner)
            {
                return from_wire(inner.error(), blockOffset);
            }

            switch (inner->number)
            {
                case kGroupNodes:
                case kGroupDense:
                    contents.hasNodes = true;
                    break;
                case kGroupWays:
                    contents.hasWays = true;
                    break;
                case kGroupRelations:
                    contents.hasRelations = true;
                    break;
                default:
                    break;
            }

            // Skipped without decoding: this is the whole point of peeking.
            if (auto skipped = group->skip(inner->wire); !skipped)
            {
                return from_wire(skipped.error(), blockOffset);
            }
        }
    }

    return contents;
}

} // namespace osm
