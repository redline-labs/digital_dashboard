// SPDX-License-Identifier: GPL-3.0-or-later
//
// Building PBF bytes longhand, from the spec.
//
// Every varint, zigzag and delta below is written out here rather than produced
// through libs/osm's own primitives. That is the whole point: a builder that
// shared the decoder's routines would agree with the decoder even where both
// are wrong, and the traps this suite exists to catch -- a zigzag written as
// division, a delta accumulated per block instead of per group -- are exactly
// the ones a shared implementation would hide. Same argument as
// libs/mvt/tests/tile_builder.h.
#ifndef OSM_TEST_PBF_BUILDER_H
#define OSM_TEST_PBF_BUILDER_H

#include <cstdint>
#include <string>
#include <vector>

#include <zlib.h>

namespace osm_test
{

using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// Wire primitives
// ---------------------------------------------------------------------------

inline void putVarint(Bytes& out, std::uint64_t value)
{
    while (value >= 0x80)
    {
        out.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

// Two's complement, sign-extended to 64 bits -- what proto int32/int64 do, and
// what makes -1 ten bytes long.
inline void putInt(Bytes& out, std::int64_t value)
{
    putVarint(out, static_cast<std::uint64_t>(value));
}

// Zigzag: (n << 1) ^ (n >> 63). Written out because the naive alternative is
// what the decoder must not be allowed to agree with.
inline void putZigzag(Bytes& out, std::int64_t value)
{
    const std::uint64_t encoded =
        (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
    putVarint(out, encoded);
}

inline void putTag(Bytes& out, std::uint32_t field, std::uint32_t wire)
{
    putVarint(out, (static_cast<std::uint64_t>(field) << 3) | wire);
}

inline void putLengthDelimited(Bytes& out, std::uint32_t field, const Bytes& payload)
{
    putTag(out, field, 2);
    putVarint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

inline void putString(Bytes& out, std::uint32_t field, const std::string& text)
{
    Bytes payload(text.begin(), text.end());
    putLengthDelimited(out, field, payload);
}

inline void putVarintField(Bytes& out, std::uint32_t field, std::int64_t value)
{
    putTag(out, field, 0);
    putInt(out, value);
}

template <typename T, typename EncodeFn>
inline Bytes packed(const std::vector<T>& values, EncodeFn encode)
{
    Bytes out;
    for (const T& value : values)
    {
        encode(out, value);
    }
    return out;
}

inline Bytes packedZigzagDeltas(const std::vector<std::int64_t>& absolute)
{
    Bytes out;
    std::int64_t previous = 0;
    for (const std::int64_t value : absolute)
    {
        putZigzag(out, value - previous);
        previous = value;
    }
    return out;
}

inline Bytes packedVarints(const std::vector<std::uint64_t>& values)
{
    Bytes out;
    for (const std::uint64_t value : values)
    {
        putVarint(out, value);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

inline Bytes deflateBytes(const Bytes& raw)
{
    Bytes out(compressBound(static_cast<uLong>(raw.size())));
    uLongf produced = static_cast<uLongf>(out.size());
    const int status = compress2(out.data(), &produced, raw.data(),
                                 static_cast<uLong>(raw.size()), Z_BEST_SPEED);
    if (status != Z_OK)
    {
        return {};
    }
    out.resize(produced);
    return out;
}

// A Blob message carrying `raw` uncompressed.
inline Bytes rawBlob(const Bytes& raw)
{
    Bytes blob;
    putLengthDelimited(blob, 1, raw);  // Blob.raw
    return blob;
}

// A Blob message carrying `raw` zlib-compressed, with raw_size set.
inline Bytes zlibBlob(const Bytes& raw)
{
    Bytes blob;
    putVarintField(blob, 2, static_cast<std::int64_t>(raw.size()));  // Blob.raw_size
    putLengthDelimited(blob, 3, deflateBytes(raw));                  // Blob.zlib_data
    return blob;
}

// One framed blob: big-endian header length, BlobHeader, then the Blob.
//
// The length prefix is the ONLY big-endian number in the format, and it is
// written out byte by byte here so the test does not depend on the decoder
// agreeing about which way round it goes.
inline Bytes framed(const std::string& type, const Bytes& blob)
{
    Bytes header;
    putString(header, 1, type);                                       // BlobHeader.type
    putVarintField(header, 3, static_cast<std::int64_t>(blob.size()));  // BlobHeader.datasize

    Bytes out;
    const auto length = static_cast<std::uint32_t>(header.size());
    out.push_back(static_cast<std::uint8_t>((length >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(length & 0xFF));
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

struct HeaderBlockSpec
{
    std::vector<std::string> requiredFeatures { "OsmSchema-V0.6", "DenseNodes" };
    std::vector<std::string> optionalFeatures;
    bool hasBbox { false };
    // Nanodegrees.
    std::int64_t left { 0 };
    std::int64_t right { 0 };
    std::int64_t top { 0 };
    std::int64_t bottom { 0 };
    std::string writingProgram;
};

inline Bytes headerBlock(const HeaderBlockSpec& spec)
{
    Bytes out;

    if (spec.hasBbox)
    {
        Bytes bbox;
        putTag(bbox, 1, 0);
        putZigzag(bbox, spec.left);
        putTag(bbox, 2, 0);
        putZigzag(bbox, spec.right);
        putTag(bbox, 3, 0);
        putZigzag(bbox, spec.top);
        putTag(bbox, 4, 0);
        putZigzag(bbox, spec.bottom);
        putLengthDelimited(out, 1, bbox);
    }

    for (const std::string& feature : spec.requiredFeatures)
    {
        putString(out, 4, feature);
    }
    for (const std::string& feature : spec.optionalFeatures)
    {
        putString(out, 5, feature);
    }
    if (!spec.writingProgram.empty())
    {
        putString(out, 16, spec.writingProgram);
    }

    return out;
}

// A node as it appears in DenseNodes: absolute values, which the builder
// deltas on the way out.
struct DenseNodeSpec
{
    std::int64_t id { 0 };
    // Raw units, before granularity and offset. With the default granularity of
    // 100 these are hundreds of nanodegrees, i.e. 1e-7 degrees.
    std::int64_t lat { 0 };
    std::int64_t lon { 0 };
    // Indices into the string table, as (key, value) pairs.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tags;
};

struct WaySpec
{
    std::int64_t id { 0 };
    std::vector<std::int64_t> refs;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tags;
};

struct MemberSpec
{
    std::int64_t ref { 0 };
    // 0 node, 1 way, 2 relation.
    std::int32_t type { 0 };
    std::uint32_t roleIndex { 0 };
};

struct RelationSpec
{
    std::int64_t id { 0 };
    std::vector<MemberSpec> members;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tags;
};

struct GroupSpec
{
    std::vector<DenseNodeSpec> dense;
    std::vector<WaySpec> ways;
    std::vector<RelationSpec> relations;
};

struct PrimitiveBlockSpec
{
    std::vector<std::string> strings { "" };  // index 0 is the empty string, by convention
    std::vector<GroupSpec> groups;
    std::int32_t granularity { 100 };
    std::int64_t latOffset { 0 };
    std::int64_t lonOffset { 0 };
    // When false, DenseNodes.keys_vals is omitted entirely -- the shape a block
    // of untagged nodes really has.
    bool emitKeysVals { true };
};

inline Bytes denseNodes(const std::vector<DenseNodeSpec>& nodes, bool emitKeysVals)
{
    std::vector<std::int64_t> ids;
    std::vector<std::int64_t> lats;
    std::vector<std::int64_t> lons;
    for (const DenseNodeSpec& node : nodes)
    {
        ids.push_back(node.id);
        lats.push_back(node.lat);
        lons.push_back(node.lon);
    }

    Bytes out;
    putLengthDelimited(out, 1, packedZigzagDeltas(ids));
    putLengthDelimited(out, 8, packedZigzagDeltas(lats));
    putLengthDelimited(out, 9, packedZigzagDeltas(lons));

    if (emitKeysVals)
    {
        // ONE FLAT ARRAY, ZERO-TERMINATED PER NODE. A node with no tags is a
        // lone 0. This is the layout that breaks a reader that assumes a list
        // per node, and it is written out here rather than generated so the
        // test states the shape it is asserting.
        Bytes keysVals;
        for (const DenseNodeSpec& node : nodes)
        {
            for (const auto& [key, value] : node.tags)
            {
                putVarint(keysVals, key);
                putVarint(keysVals, value);
            }
            putVarint(keysVals, 0);
        }
        putLengthDelimited(out, 10, keysVals);
    }

    return out;
}

inline Bytes way(const WaySpec& spec)
{
    Bytes out;
    putVarintField(out, 1, spec.id);  // int64, not zigzag

    if (!spec.tags.empty())
    {
        std::vector<std::uint64_t> keys;
        std::vector<std::uint64_t> vals;
        for (const auto& [key, value] : spec.tags)
        {
            keys.push_back(key);
            vals.push_back(value);
        }
        putLengthDelimited(out, 2, packedVarints(keys));
        putLengthDelimited(out, 3, packedVarints(vals));
    }

    // Refs are DELTA-CODED zigzag, unlike the way id above them.
    putLengthDelimited(out, 8, packedZigzagDeltas(spec.refs));
    return out;
}

inline Bytes relation(const RelationSpec& spec)
{
    Bytes out;
    putVarintField(out, 1, spec.id);

    if (!spec.tags.empty())
    {
        std::vector<std::uint64_t> keys;
        std::vector<std::uint64_t> vals;
        for (const auto& [key, value] : spec.tags)
        {
            keys.push_back(key);
            vals.push_back(value);
        }
        putLengthDelimited(out, 2, packedVarints(keys));
        putLengthDelimited(out, 3, packedVarints(vals));
    }

    std::vector<std::uint64_t> roles;
    std::vector<std::int64_t> memids;
    std::vector<std::uint64_t> types;
    for (const MemberSpec& member : spec.members)
    {
        roles.push_back(member.roleIndex);
        memids.push_back(member.ref);
        types.push_back(static_cast<std::uint64_t>(member.type));
    }

    // Roles and types are plain, member ids are delta-coded. The asymmetry is
    // real and is one of the easy things to get wrong.
    putLengthDelimited(out, 8, packedVarints(roles));
    putLengthDelimited(out, 9, packedZigzagDeltas(memids));
    putLengthDelimited(out, 10, packedVarints(types));
    return out;
}

inline Bytes primitiveBlock(const PrimitiveBlockSpec& spec)
{
    Bytes out;

    Bytes table;
    for (const std::string& text : spec.strings)
    {
        putString(table, 1, text);
    }
    putLengthDelimited(out, 1, table);

    for (const GroupSpec& group : spec.groups)
    {
        Bytes bytes;
        if (!group.dense.empty())
        {
            putLengthDelimited(bytes, 2, denseNodes(group.dense, spec.emitKeysVals));
        }
        for (const WaySpec& w : group.ways)
        {
            putLengthDelimited(bytes, 3, way(w));
        }
        for (const RelationSpec& r : group.relations)
        {
            putLengthDelimited(bytes, 4, relation(r));
        }
        putLengthDelimited(out, 2, bytes);
    }

    if (spec.granularity != 100)
    {
        putVarintField(out, 17, spec.granularity);
    }
    if (spec.latOffset != 0)
    {
        putVarintField(out, 19, spec.latOffset);
    }
    if (spec.lonOffset != 0)
    {
        putVarintField(out, 20, spec.lonOffset);
    }

    return out;
}

} // namespace osm_test

#endif // OSM_TEST_PBF_BUILDER_H
