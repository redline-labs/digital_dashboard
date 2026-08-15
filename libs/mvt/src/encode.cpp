// SPDX-License-Identifier: GPL-3.0-or-later
#include "mvt/encode.h"

#include <cstring>

#include <zlib.h>

namespace mvt
{
namespace
{

// Field numbers, from vector_tile.proto. The same eleven fields decode.cpp
// reads.
constexpr std::uint32_t kTileLayer = 3;

constexpr std::uint32_t kLayerName = 1;
constexpr std::uint32_t kLayerFeature = 2;
constexpr std::uint32_t kLayerKey = 3;
constexpr std::uint32_t kLayerValue = 4;
constexpr std::uint32_t kLayerExtent = 5;
constexpr std::uint32_t kLayerVersion = 15;

constexpr std::uint32_t kFeatureId = 1;
constexpr std::uint32_t kFeatureTags = 2;
constexpr std::uint32_t kFeatureType = 3;
constexpr std::uint32_t kFeatureGeometry = 4;

constexpr std::uint32_t kValueString = 1;
constexpr std::uint32_t kValueDouble = 3;
constexpr std::uint32_t kValueInt = 4;
constexpr std::uint32_t kValueBool = 7;

// Geometry commands.
constexpr std::uint32_t kMoveTo = 1;
constexpr std::uint32_t kLineTo = 2;
constexpr std::uint32_t kClosePath = 7;

using Bytes = std::vector<std::uint8_t>;

void putVarint(Bytes& out, std::uint64_t value)
{
    while (value >= 0x80)
    {
        out.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

// (n << 1) ^ (n >> 63). Written out because the naive alternative is exactly
// what unzigzag() in protowire exists to catch, and an encoder that got it
// wrong would produce a tile that decoded into a mirrored shape.
void putZigzag(Bytes& out, std::int64_t value)
{
    putVarint(out, (static_cast<std::uint64_t>(value) << 1) ^
                       static_cast<std::uint64_t>(value >> 63));
}

void putTag(Bytes& out, std::uint32_t field, std::uint32_t wire)
{
    putVarint(out, (static_cast<std::uint64_t>(field) << 3) | wire);
}

void putVarintField(Bytes& out, std::uint32_t field, std::uint64_t value)
{
    putTag(out, field, 0);
    putVarint(out, value);
}

void putBytesField(Bytes& out, std::uint32_t field, const Bytes& payload)
{
    putTag(out, field, 2);
    putVarint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

void putStringField(Bytes& out, std::uint32_t field, const std::string& text)
{
    putTag(out, field, 2);
    putVarint(out, text.size());
    out.insert(out.end(), text.begin(), text.end());
}

void putDoubleField(Bytes& out, std::uint32_t field, double value)
{
    putTag(out, field, 1);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
    }
}

Bytes encodeValue(const Value& value)
{
    Bytes out;
    if (const auto* text = std::get_if<std::string>(&value); text != nullptr)
    {
        putStringField(out, kValueString, *text);
    }
    else if (const auto* number = std::get_if<double>(&value); number != nullptr)
    {
        putDoubleField(out, kValueDouble, *number);
    }
    else if (const auto* integer = std::get_if<std::int64_t>(&value); integer != nullptr)
    {
        putVarintField(out, kValueInt, static_cast<std::uint64_t>(*integer));
    }
    else if (const auto* flag = std::get_if<bool>(&value); flag != nullptr)
    {
        putVarintField(out, kValueBool, *flag ? 1u : 0u);
    }
    // std::monostate encodes as an empty Value, which is legal and decodes back
    // to monostate.
    return out;
}

Bytes encodeGeometry(const Feature& feature)
{
    Bytes out;

    // THE CURSOR PERSISTS ACROSS COMMANDS. Every point is a delta from the
    // last one written, including the first point of a later ring. Resetting it
    // per ring -- the obvious mistake -- produces a tile whose second and
    // subsequent rings are drawn relative to the tile corner.
    std::int32_t cursorX = 0;
    std::int32_t cursorY = 0;

    for (const std::vector<Point>& ring : feature.rings)
    {
        if (ring.empty())
        {
            continue;
        }

        // A POLYGON RING THAT WOULD ENCODE TO FEWER THAN THREE POINTS IS
        // DROPPED, and this is the last line of defence for the whole tile.
        //
        // The closing point is implied by ClosePath and is stripped below, so a
        // caller's three-point ring [A,B,A] -- which is what a sliver becomes
        // once quantised to tile units -- would go out as two points. That
        // makes the TILE malformed, not the feature: a decoder that rejects it
        // discards every road, label and coastline in the same square, and the
        // map reports no coverage over a tileset that has the data. The cost of
        // being wrong is a whole tile, so the check lives here, where nothing
        // can encode past it.
        if (feature.type == GeomType::Polygon)
        {
            std::size_t distinct = ring.size();
            if (distinct > 1 && ring.front() == ring.back())
            {
                --distinct;
            }
            if (distinct < 3)
            {
                continue;
            }
        }

        putVarint(out, (1u << 3) | kMoveTo);
        putZigzag(out, ring[0].x - cursorX);
        putZigzag(out, ring[0].y - cursorY);
        cursorX = ring[0].x;
        cursorY = ring[0].y;

        // A polygon ring's closing point is implied by ClosePath and must NOT
        // be written: a ring whose last point repeats its first decodes as a
        // zero-length final edge, which some renderers draw as a spike.
        std::size_t count = ring.size() - 1;
        if (feature.type == GeomType::Polygon && count > 0 &&
            ring.front() == ring[ring.size() - 1])
        {
            --count;
        }

        if (count > 0)
        {
            putVarint(out, (static_cast<std::uint32_t>(count) << 3) | kLineTo);
            for (std::size_t i = 1; i <= count; ++i)
            {
                putZigzag(out, ring[i].x - cursorX);
                putZigzag(out, ring[i].y - cursorY);
                cursorX = ring[i].x;
                cursorY = ring[i].y;
            }
        }

        if (feature.type == GeomType::Polygon)
        {
            putVarint(out, (1u << 3) | kClosePath);
        }
    }

    return out;
}

Bytes encodeFeature(const Feature& feature)
{
    Bytes out;

    if (feature.hasId)
    {
        putVarintField(out, kFeatureId, feature.id);
    }

    if (!feature.tags.empty())
    {
        Bytes tags;
        for (const std::uint32_t index : feature.tags)
        {
            putVarint(tags, index);
        }
        putBytesField(out, kFeatureTags, tags);
    }

    putVarintField(out, kFeatureType, static_cast<std::uint64_t>(feature.type));

    const Bytes geometry = encodeGeometry(feature);
    if (!geometry.empty())
    {
        putBytesField(out, kFeatureGeometry, geometry);
    }

    return out;
}

Bytes encodeLayer(const Layer& layer)
{
    Bytes out;

    // Version first: a decoder that reads fields in order sees the version
    // before anything it might have to interpret differently because of it.
    putVarintField(out, kLayerVersion, layer.version == 0 ? 2 : layer.version);
    putStringField(out, kLayerName, layer.name);

    for (const Feature& feature : layer.features)
    {
        // A feature every one of whose rings was dropped above has nothing to
        // draw. Writing it anyway leaves a feature with no geometry, which the
        // spec permits and no renderer wants.
        if (!feature.rings.empty() && encodeGeometry(feature).empty())
        {
            continue;
        }
        putBytesField(out, kLayerFeature, encodeFeature(feature));
    }
    for (const std::string& key : layer.keys)
    {
        putStringField(out, kLayerKey, key);
    }
    for (const Value& value : layer.values)
    {
        putBytesField(out, kLayerValue, encodeValue(value));
    }

    // Written even when it is 4096. Being explicit costs three bytes and
    // removes a class of "why is this layer at the wrong scale" question.
    putVarintField(out, kLayerExtent, layer.extent == 0 ? 4096 : layer.extent);

    return out;
}

} // namespace

Result<std::vector<std::uint8_t>> encode(const Tile& tile)
{
    Bytes out;
    for (const Layer& layer : tile.layers)
    {
        if (layer.name.empty())
        {
            return malformed("a layer with no name");
        }
        for (const Feature& feature : layer.features)
        {
            if (feature.tags.size() % 2 != 0)
            {
                return malformed("a feature with an odd number of tag indices");
            }
            for (std::size_t i = 0; i < feature.tags.size(); i += 2)
            {
                if (feature.tags[i] >= layer.keys.size() ||
                    feature.tags[i + 1] >= layer.values.size())
                {
                    // Refused rather than written. A tag index past the end
                    // decodes to a different attribute or to nothing, and the
                    // tile still renders -- so the failure would only ever show
                    // up as a road with somebody else's name on it.
                    return malformed("a tag index past the end of the layer's key or value table");
                }
            }
        }
        putBytesField(out, kTileLayer, encodeLayer(layer));
    }
    return out;
}

Result<std::vector<std::uint8_t>> gzipCompress(std::span<const std::uint8_t> bytes)
{
    z_stream stream {};
    // 15 + 16 selects a gzip wrapper rather than zlib's own. The spec says
    // gzip, and mvt::inflate auto-detects both -- but writing zlib here would
    // produce archives that every other tool refuses.
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK)
    {
        return decompress_failed("zlib would not start");
    }

    std::vector<std::uint8_t> out(deflateBound(&stream, static_cast<uLong>(bytes.size())));
    stream.next_in = const_cast<Bytef*>(bytes.data());
    stream.avail_in = static_cast<uInt>(bytes.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());

    const int status = deflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    deflateEnd(&stream);

    if (status != Z_STREAM_END)
    {
        return decompress_failed("zlib refused to compress the tile");
    }

    out.resize(produced);
    return out;
}

} // namespace mvt
