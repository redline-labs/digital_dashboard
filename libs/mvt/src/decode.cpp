// SPDX-License-Identifier: GPL-3.0-or-later
#include "mvt/decode.h"

#include <algorithm>

#include "mvt/reader.h"

#include <cstring>
#include <limits>
#include <utility>

namespace mvt
{
namespace
{

// Field numbers from the vector_tile.proto (MVT 2.1). Written out rather than
// left as literals at the switch sites, because a wrong number does not fail --
// it reads a different field and yields a tile that is merely wrong.
namespace tile_field
{
constexpr std::uint32_t kLayer = 3;
}

namespace layer_field
{
constexpr std::uint32_t kName = 1;
constexpr std::uint32_t kFeature = 2;
constexpr std::uint32_t kKey = 3;
constexpr std::uint32_t kValue = 4;
constexpr std::uint32_t kExtent = 5;
// 15, not 6. The spec puts version last on purpose so it survives a reader
// that stops early, and the gap is the single most likely thing to mistype.
constexpr std::uint32_t kVersion = 15;
}

namespace feature_field
{
constexpr std::uint32_t kId = 1;
constexpr std::uint32_t kTags = 2;
constexpr std::uint32_t kType = 3;
constexpr std::uint32_t kGeometry = 4;
}

namespace value_field
{
constexpr std::uint32_t kString = 1;
constexpr std::uint32_t kFloat = 2;
constexpr std::uint32_t kDouble = 3;
constexpr std::uint32_t kInt64 = 4;
constexpr std::uint32_t kUint64 = 5;
constexpr std::uint32_t kSint64 = 6;
constexpr std::uint32_t kBool = 7;
}

// Geometry command ids.
constexpr std::uint32_t kMoveTo = 1;
constexpr std::uint32_t kLineTo = 2;
constexpr std::uint32_t kClosePath = 7;

// A tile with more geometry than this is not a tile we can have produced, and
// decoding it would let a malformed length allocate without bound. The largest
// tile in the SoCal archive is ~300 kB; a million points is far above anything
// real and far below anything dangerous.
constexpr std::size_t kMaxPointsPerFeature = 1'000'000;

Result<Value> decodeValue(Reader reader)
{
    Value out;

    while (!reader.done())
    {
        auto field = reader.field();
        if (!field)
        {
            return std::unexpected(field.error());
        }

        switch (field->number)
        {
            case value_field::kString:
            {
                auto text = reader.text();
                if (!text)
                {
                    return std::unexpected(text.error());
                }
                out = std::string(*text);
                break;
            }

            case value_field::kFloat:
            {
                auto raw = reader.fixed32();
                if (!raw)
                {
                    return std::unexpected(raw.error());
                }
                float value = 0.0F;
                static_assert(sizeof(value) == sizeof(std::uint32_t));
                std::memcpy(&value, &*raw, sizeof(value));
                out = static_cast<double>(value);
                break;
            }

            case value_field::kDouble:
            {
                auto raw = reader.fixed64();
                if (!raw)
                {
                    return std::unexpected(raw.error());
                }
                double value = 0.0;
                static_assert(sizeof(value) == sizeof(std::uint64_t));
                std::memcpy(&value, &*raw, sizeof(value));
                out = value;
                break;
            }

            case value_field::kInt64:
            case value_field::kUint64:
            {
                auto raw = reader.varint();
                if (!raw)
                {
                    return std::unexpected(raw.error());
                }
                out = static_cast<std::int64_t>(*raw);
                break;
            }

            case value_field::kSint64:
            {
                auto raw = reader.zigzag();
                if (!raw)
                {
                    return std::unexpected(raw.error());
                }
                out = *raw;
                break;
            }

            case value_field::kBool:
            {
                auto raw = reader.varint();
                if (!raw)
                {
                    return std::unexpected(raw.error());
                }
                out = (*raw != 0);
                break;
            }

            default:
            {
                // Unknown fields are SKIPPED, not rejected. That is the
                // forward-compatibility rule in the spec, and a decoder that
                // errors here breaks on the next revision of a format it would
                // otherwise have read perfectly well.
                auto skipped = reader.skip(field->wire);
                if (!skipped)
                {
                    return std::unexpected(skipped.error());
                }
                break;
            }
        }
    }

    return out;
}

// The geometry command stream: the one genuinely unusual part of the format.
//
// A packed uint32 array where each "command integer" is (id | count << 3),
// followed by count * (2 for MoveTo/LineTo, 0 for ClosePath) zigzag varints.
// Coordinates are DELTAS from the previous point, and the cursor persists
// across commands -- so a decoder that resets it per command produces geometry
// collapsed onto the origin.
Result<std::vector<std::vector<Point>>> decodeGeometry(Reader reader, GeomType type)
{
    std::vector<std::vector<Point>> rings;
    std::vector<Point> current;

    std::int64_t cursorX = 0;
    std::int64_t cursorY = 0;
    std::size_t totalPoints = 0;

    while (!reader.done())
    {
        const std::size_t at = reader.offset();

        auto command = reader.varint();
        if (!command)
        {
            return std::unexpected(command.error());
        }

        const std::uint32_t id = static_cast<std::uint32_t>(*command) & 0x07U;
        const std::uint64_t count = *command >> 3;

        if (id == kClosePath)
        {
            // ClosePath takes a count but no parameters, and the spec fixes the
            // count at 1. The ring is closed implicitly -- the first point is
            // NOT repeated, which is why signedArea2 wraps with a modulo.
            if (count != 1)
            {
                return malformed("ClosePath with count " + std::to_string(count), at);
            }
            if (current.empty())
            {
                return malformed("ClosePath with no open ring", at);
            }
            rings.push_back(std::move(current));
            current.clear();
            continue;
        }

        if (id != kMoveTo && id != kLineTo)
        {
            return malformed("geometry command id " + std::to_string(id), at);
        }

        if (count == 0)
        {
            return malformed("geometry command with zero count", at);
        }

        totalPoints += static_cast<std::size_t>(count);
        if (totalPoints > kMaxPointsPerFeature)
        {
            return malformed("feature claims more than " +
                                 std::to_string(kMaxPointsPerFeature) + " points",
                             at);
        }

        // RESERVE, because this is where the decoder spends its time. A sample
        // of the widget's tile-load path showed the allocator -- malloc, free,
        // memset -- outweighing every parsing symbol in it: the points of a
        // ring were being appended one at a time into a vector that regrew as
        // it went, once per ring, once per feature, once per tile.
        //
        // Bounded twice over, because `count` comes off disk: the cumulative
        // check above caps it at kMaxPointsPerFeature, and each point costs at
        // least two bytes of varint, so a truncated tile claiming a huge count
        // cannot make us reserve more than the bytes that are actually left.
        // Only for LineTo -- a MoveTo may flush and start a new ring per point.
        if (id == kLineTo)
        {
            const std::size_t possible = std::min<std::size_t>(
                static_cast<std::size_t>(count), reader.remaining() / 2U);
            current.reserve(current.size() + possible);
        }

        for (std::uint64_t i = 0; i < count; ++i)
        {
            auto dx = reader.zigzag();
            if (!dx)
            {
                return std::unexpected(dx.error());
            }
            auto dy = reader.zigzag();
            if (!dy)
            {
                return std::unexpected(dy.error());
            }

            // Accumulated in int64 and range-checked before narrowing. The
            // deltas are attacker-controlled in the sense that they come off
            // disk, and int32 overflow here is UB rather than a wrong pixel.
            cursorX += *dx;
            cursorY += *dy;
            if (cursorX < std::numeric_limits<std::int32_t>::min() ||
                cursorX > std::numeric_limits<std::int32_t>::max() ||
                cursorY < std::numeric_limits<std::int32_t>::min() ||
                cursorY > std::numeric_limits<std::int32_t>::max())
            {
                return malformed("geometry cursor left int32 range", at);
            }

            if (id == kMoveTo)
            {
                // A MoveTo begins a new part. For a MultiPoint the count is
                // greater than one and every point is its own part, which is
                // why the flush happens per point rather than per command.
                if (!current.empty())
                {
                    rings.push_back(std::move(current));
                    current.clear();
                }
            }

            current.push_back(Point { static_cast<std::int32_t>(cursorX),
                                      static_cast<std::int32_t>(cursorY) });
        }
    }

    if (!current.empty())
    {
        rings.push_back(std::move(current));
    }

    // A polygon must have been closed. An unclosed one means the stream was
    // cut, and drawing it would produce a shape that is filled to somewhere
    // arbitrary rather than an obvious gap.
    if (type == GeomType::Polygon)
    {
        for (const auto& ring : rings)
        {
            if (ring.size() < 3)
            {
                return malformed("polygon ring with " + std::to_string(ring.size()) + " points");
            }
        }
    }

    return rings;
}

Result<Feature> decodeFeature(Reader reader)
{
    Feature feature;
    std::span<const std::uint8_t> geometry;

    while (!reader.done())
    {
        auto field = reader.field();
        if (!field)
        {
            return std::unexpected(field.error());
        }

        switch (field->number)
        {
            case feature_field::kId:
            {
                auto id = reader.varint();
                if (!id)
                {
                    return std::unexpected(id.error());
                }
                feature.id = *id;
                feature.hasId = true;
                break;
            }

            case feature_field::kTags:
            {
                // Packed repeated uint32. It may also legally appear unpacked,
                // one varint per tag -- which is what the wire type says, so
                // both are handled rather than assumed.
                if (field->wire == WireType::LengthDelimited)
                {
                    auto packed = reader.sub();
                    if (!packed)
                    {
                        return std::unexpected(packed.error());
                    }
                    while (!packed->done())
                    {
                        auto tag = packed->varint();
                        if (!tag)
                        {
                            return std::unexpected(tag.error());
                        }
                        feature.tags.push_back(static_cast<std::uint32_t>(*tag));
                    }
                }
                else
                {
                    auto tag = reader.varint();
                    if (!tag)
                    {
                        return std::unexpected(tag.error());
                    }
                    feature.tags.push_back(static_cast<std::uint32_t>(*tag));
                }
                break;
            }

            case feature_field::kType:
            {
                auto type = reader.varint();
                if (!type)
                {
                    return std::unexpected(type.error());
                }
                if (*type > static_cast<std::uint64_t>(GeomType::Polygon))
                {
                    // An unknown geometry type is not fatal: the feature is
                    // simply not drawable, and dropping the tile for it would
                    // lose every other feature in it.
                    feature.type = GeomType::Unknown;
                }
                else
                {
                    feature.type = static_cast<GeomType>(*type);
                }
                break;
            }

            case feature_field::kGeometry:
            {
                auto packed = reader.bytes();
                if (!packed)
                {
                    return std::unexpected(packed.error());
                }
                geometry = *packed;
                break;
            }

            default:
            {
                auto skipped = reader.skip(field->wire);
                if (!skipped)
                {
                    return std::unexpected(skipped.error());
                }
                break;
            }
        }
    }

    // Tags come in (key, value) pairs. An odd count means one of them has no
    // partner, and pairing up regardless would attach every later attribute to
    // the wrong key -- a feature that reads as a river because the offset
    // slipped by one.
    if ((feature.tags.size() % 2) != 0)
    {
        return malformed("feature has " + std::to_string(feature.tags.size()) +
                         " tags, which is not a whole number of pairs");
    }

    // The type field can legally come after the geometry, so geometry is
    // decoded here rather than in the loop.
    if (!geometry.empty())
    {
        auto rings = decodeGeometry(Reader(geometry), feature.type);
        if (!rings)
        {
            return std::unexpected(rings.error());
        }
        feature.rings = std::move(*rings);
    }

    return feature;
}

Result<Layer> decodeLayer(Reader reader)
{
    Layer layer;
    bool sawExtent = false;

    while (!reader.done())
    {
        auto field = reader.field();
        if (!field)
        {
            return std::unexpected(field.error());
        }

        switch (field->number)
        {
            case layer_field::kName:
            {
                auto name = reader.text();
                if (!name)
                {
                    return std::unexpected(name.error());
                }
                layer.name = std::string(*name);
                break;
            }

            case layer_field::kFeature:
            {
                auto sub = reader.sub();
                if (!sub)
                {
                    return std::unexpected(sub.error());
                }
                auto feature = decodeFeature(*sub);
                if (!feature)
                {
                    return std::unexpected(feature.error());
                }
                layer.features.push_back(std::move(*feature));
                break;
            }

            case layer_field::kKey:
            {
                auto key = reader.text();
                if (!key)
                {
                    return std::unexpected(key.error());
                }
                layer.keys.emplace_back(*key);
                break;
            }

            case layer_field::kValue:
            {
                auto sub = reader.sub();
                if (!sub)
                {
                    return std::unexpected(sub.error());
                }
                auto value = decodeValue(*sub);
                if (!value)
                {
                    return std::unexpected(value.error());
                }
                layer.values.push_back(std::move(*value));
                break;
            }

            case layer_field::kExtent:
            {
                auto extent = reader.varint();
                if (!extent)
                {
                    return std::unexpected(extent.error());
                }
                if (*extent == 0 || *extent > 0xFFFFFFFFULL)
                {
                    // Extent is the divisor in every coordinate transform.
                    // Zero would be a division by zero at paint time, several
                    // layers away from here.
                    return malformed("layer extent " + std::to_string(*extent));
                }
                layer.extent = static_cast<std::uint32_t>(*extent);
                sawExtent = true;
                break;
            }

            case layer_field::kVersion:
            {
                auto version = reader.varint();
                if (!version)
                {
                    return std::unexpected(version.error());
                }
                layer.version = static_cast<std::uint32_t>(*version);
                break;
            }

            default:
            {
                auto skipped = reader.skip(field->wire);
                if (!skipped)
                {
                    return std::unexpected(skipped.error());
                }
                break;
            }
        }
    }

    if (!sawExtent)
    {
        // The proto default. A layer that omits extent means 4096, and a
        // decoder that leaves it zero divides by zero at paint time.
        layer.extent = 4096;
    }

    // Every tag index must be in range. Checked once here rather than at every
    // lookup: an out-of-range index is a corrupt tile, and finding out at paint
    // time means an out-of-bounds read in the hot path.
    for (const Feature& feature : layer.features)
    {
        for (std::size_t i = 0; i + 1 < feature.tags.size(); i += 2)
        {
            if (feature.tags[i] >= layer.keys.size())
            {
                return malformed("tag key index " + std::to_string(feature.tags[i]) +
                                 " past " + std::to_string(layer.keys.size()) + " keys");
            }
            if (feature.tags[i + 1] >= layer.values.size())
            {
                return malformed("tag value index " + std::to_string(feature.tags[i + 1]) +
                                 " past " + std::to_string(layer.values.size()) + " values");
            }
        }
    }

    return layer;
}

} // namespace

bool looksCompressed(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B)
    {
        return true;
    }
    // zlib: deflate method in the low nibble, and a two-byte header that is a
    // multiple of 31.
    return bytes.size() >= 2 && (bytes[0] & 0x0F) == 0x08 &&
           (((static_cast<unsigned>(bytes[0]) << 8) | bytes[1]) % 31U) == 0U;
}

Result<Tile> decode(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
    {
        return Tile {};
    }

    // Checked by name because the failure otherwise is misleading. A gzip
    // stream starts 0x1F 0x8B, which protobuf reads as field 3 wire type 7 --
    // so the error would say "wire type 7" and send the reader looking at the
    // decoder rather than at the missing inflate.
    if (looksCompressed(bytes))
    {
        return malformed("input is still compressed; inflate it first (see mvt/gzip.h)");
    }

    Tile tile;
    Reader reader(bytes);

    while (!reader.done())
    {
        auto field = reader.field();
        if (!field)
        {
            return std::unexpected(field.error());
        }

        if (field->number == tile_field::kLayer)
        {
            auto sub = reader.sub();
            if (!sub)
            {
                return std::unexpected(sub.error());
            }
            auto layer = decodeLayer(*sub);
            if (!layer)
            {
                return std::unexpected(layer.error());
            }
            tile.layers.push_back(std::move(*layer));
            continue;
        }

        auto skipped = reader.skip(field->wire);
        if (!skipped)
        {
            return std::unexpected(skipped.error());
        }
    }

    return tile;
}

} // namespace mvt
