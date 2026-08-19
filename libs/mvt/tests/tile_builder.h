// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hand-built vector tiles, for tests.
//
// Deliberately an ENCODER, not a round-trip through the decoder's own helpers.
// A test that built its bytes with the same varint and zigzag routines it is
// checking would agree with the decoder even where both are wrong -- the same
// argument libs/gsof/tests/golden/README.md makes for using captures from real
// receivers rather than vectors authored from the same reading of the ICD.
//
// So the encoding here is written out longhand from the protobuf and MVT specs,
// and the numbers in the tests below are ones a human worked out. The other
// half of the argument is tests/test_real_tiles.cpp, which decodes bytes nobody
// in this repository wrote at all.
#ifndef MVT_TESTS_TILE_BUILDER_H
#define MVT_TESTS_TILE_BUILDER_H

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mvt_test
{

using Bytes = std::vector<std::uint8_t>;

// Longhand base-128 varint: seven bits at a time, low group first, high bit set
// on every group but the last.
inline void putVarint(Bytes& out, std::uint64_t value)
{
    while (value >= 0x80)
    {
        out.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

// Longhand zigzag: non-negative n becomes 2n, negative n becomes -2n-1. Written
// as a branch rather than the bit trick so it is obviously the definition from
// the spec rather than a transcription of the decoder.
inline void putZigzag(Bytes& out, std::int64_t value)
{
    const std::uint64_t encoded = (value >= 0) ? (static_cast<std::uint64_t>(value) * 2)
                                               : (static_cast<std::uint64_t>(-value) * 2) - 1;
    putVarint(out, encoded);
}

inline void putTag(Bytes& out, std::uint32_t fieldNumber, std::uint32_t wireType)
{
    putVarint(out, (static_cast<std::uint64_t>(fieldNumber) << 3) | wireType);
}

inline void putLengthDelimited(Bytes& out, std::uint32_t fieldNumber, const Bytes& payload)
{
    putTag(out, fieldNumber, 2);
    putVarint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

inline void putString(Bytes& out, std::uint32_t fieldNumber, std::string_view text)
{
    putTag(out, fieldNumber, 2);
    putVarint(out, text.size());
    out.insert(out.end(), text.begin(), text.end());
}

inline void putVarintField(Bytes& out, std::uint32_t fieldNumber, std::uint64_t value)
{
    putTag(out, fieldNumber, 0);
    putVarint(out, value);
}

// ---------------------------------------------------------------- geometry

// A geometry command integer: the id in the low three bits, the repeat count
// above it.
inline std::uint32_t command(std::uint32_t id, std::uint32_t count)
{
    return id | (count << 3);
}

constexpr std::uint32_t kMoveTo = 1;
constexpr std::uint32_t kLineTo = 2;
constexpr std::uint32_t kClosePath = 7;

// Build a geometry stream from absolute points, doing the delta encoding here.
// `counts` says how many points belong to each part; a MoveTo starts each one.
class Geometry
{
  public:
    // One MoveTo of `points.size()` points -- a MultiPoint when more than one.
    Geometry& moveTo(const std::vector<std::pair<std::int32_t, std::int32_t>>& points)
    {
        emit(kMoveTo, points);
        return *this;
    }

    Geometry& lineTo(const std::vector<std::pair<std::int32_t, std::int32_t>>& points)
    {
        emit(kLineTo, points);
        return *this;
    }

    Geometry& closePath()
    {
        putVarint(mBytes, command(kClosePath, 1));
        return *this;
    }

    // Escape hatch for the malformed cases: emit a command integer with no
    // parameters, or with the wrong number of them.
    Geometry& rawCommand(std::uint32_t id, std::uint32_t count)
    {
        putVarint(mBytes, command(id, count));
        return *this;
    }

    Geometry& rawDelta(std::int32_t dx, std::int32_t dy)
    {
        putZigzag(mBytes, dx);
        putZigzag(mBytes, dy);
        return *this;
    }

    const Bytes& bytes() const { return mBytes; }

  private:
    void emit(std::uint32_t id, const std::vector<std::pair<std::int32_t, std::int32_t>>& points)
    {
        putVarint(mBytes, command(id, static_cast<std::uint32_t>(points.size())));
        for (const auto& [x, y] : points)
        {
            putZigzag(mBytes, x - mCursorX);
            putZigzag(mBytes, y - mCursorY);
            mCursorX = x;
            mCursorY = y;
        }
    }

    Bytes mBytes;
    std::int32_t mCursorX { 0 };
    std::int32_t mCursorY { 0 };
};

// ------------------------------------------------------------------ tile

struct FeatureSpec
{
    std::uint32_t type { 2 };  // 1 point, 2 linestring, 3 polygon
    Bytes geometry;
    std::vector<std::uint32_t> tags {};
    std::uint64_t id { 0 };
    bool hasId { false };
    // Emit the tags one varint per field rather than packed. Both are legal.
    bool unpackedTags { false };
};

class LayerBuilder
{
  public:
    explicit LayerBuilder(std::string name) : mName(std::move(name)) {}

    LayerBuilder& version(std::uint32_t value)
    {
        mVersion = value;
        return *this;
    }

    LayerBuilder& extent(std::uint32_t value)
    {
        mExtent = value;
        mHasExtent = true;
        return *this;
    }

    // Omit the extent field entirely, so the proto default of 4096 applies.
    LayerBuilder& withoutExtent()
    {
        mHasExtent = false;
        return *this;
    }

    LayerBuilder& key(std::string_view name)
    {
        mKeys.emplace_back(name);
        return *this;
    }

    LayerBuilder& stringValue(std::string_view text)
    {
        Bytes value;
        putString(value, 1, text);
        mValues.push_back(std::move(value));
        return *this;
    }

    LayerBuilder& boolValue(bool flag)
    {
        Bytes value;
        putVarintField(value, 7, flag ? 1U : 0U);
        mValues.push_back(std::move(value));
        return *this;
    }

    LayerBuilder& sintValue(std::int64_t number)
    {
        Bytes value;
        putTag(value, 6, 0);
        putZigzag(value, number);
        mValues.push_back(std::move(value));
        return *this;
    }

    LayerBuilder& doubleValue(double number)
    {
        Bytes value;
        putTag(value, 3, 1);
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(number));
        std::memcpy(&bits, &number, sizeof(bits));
        for (int i = 0; i < 8; ++i)
        {
            value.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
        }
        mValues.push_back(std::move(value));
        return *this;
    }

    LayerBuilder& feature(FeatureSpec spec)
    {
        mFeatures.push_back(std::move(spec));
        return *this;
    }

    // An unrecognised field, to prove unknown fields are skipped rather than
    // rejected. Field 42 is not in the MVT proto and never will be.
    LayerBuilder& unknownField()
    {
        mUnknownField = true;
        return *this;
    }

    Bytes bytes() const
    {
        Bytes out;
        putString(out, 1, mName);

        if (mUnknownField)
        {
            putString(out, 42, "a field from the future");
        }

        for (const FeatureSpec& spec : mFeatures)
        {
            Bytes feature;
            if (spec.hasId)
            {
                putVarintField(feature, 1, spec.id);
            }

            if (!spec.tags.empty())
            {
                if (spec.unpackedTags)
                {
                    for (const std::uint32_t tag : spec.tags)
                    {
                        putVarintField(feature, 2, tag);
                    }
                }
                else
                {
                    Bytes packed;
                    for (const std::uint32_t tag : spec.tags)
                    {
                        putVarint(packed, tag);
                    }
                    putLengthDelimited(feature, 2, packed);
                }
            }

            putVarintField(feature, 3, spec.type);
            putLengthDelimited(feature, 4, spec.geometry);
            putLengthDelimited(out, 2, feature);
        }

        for (const std::string& key : mKeys)
        {
            putString(out, 3, key);
        }
        for (const Bytes& value : mValues)
        {
            putLengthDelimited(out, 4, value);
        }

        if (mHasExtent)
        {
            putVarintField(out, 5, mExtent);
        }

        // Version is field 15, deliberately last, as the spec has it.
        putVarintField(out, 15, mVersion);
        return out;
    }

  private:
    std::string mName;
    std::uint32_t mVersion { 2 };
    std::uint32_t mExtent { 4096 };
    bool mHasExtent { true };
    bool mUnknownField { false };
    std::vector<std::string> mKeys;
    std::vector<Bytes> mValues;
    std::vector<FeatureSpec> mFeatures;
};

inline Bytes tileOf(const std::vector<Bytes>& layers)
{
    Bytes out;
    for (const Bytes& layer : layers)
    {
        putLengthDelimited(out, 3, layer);
    }
    return out;
}

} // namespace mvt_test

#endif // MVT_TESTS_TILE_BUILDER_H
