// SPDX-License-Identifier: GPL-3.0-or-later
//
// A decoded Mapbox Vector Tile.
//
// Coordinates stay in TILE-LOCAL integer space, 0..extent, exactly as the wire
// has them. They are NOT projected here: the projection depends on where the
// tile is being drawn and at what size, which is the renderer's business, and
// converting twice is how a map ends up subtly offset.
//
// Three things about the format are easy to get wrong and are handled here
// rather than by every caller:
//
//   * `extent` is PER LAYER and defaults to 4096. Most tiles use 4096 and a
//     renderer that hardcodes it works until it meets one that does not, then
//     draws that layer at the wrong scale.
//
//   * Coordinates may fall OUTSIDE 0..extent. Tiles carry a buffer so a line
//     crossing the edge joins up with its neighbour; clamping or rejecting
//     those points puts a seam down every tile boundary.
//
//   * Polygon ring winding decides exterior from interior. In MVT's coordinate
//     system (y down) a positive signed area is an exterior ring and a negative
//     one is a hole. Ignoring it fills every hole in solid.
#ifndef MVT_TILE_H
#define MVT_TILE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mvt
{

// Tile-local integer coordinates. Signed because the buffer puts real points
// outside the tile on both sides.
struct Point
{
    std::int32_t x { 0 };
    std::int32_t y { 0 };

    friend bool operator==(const Point&, const Point&) = default;
};

enum class GeomType : std::uint8_t
{
    Unknown = 0,
    Point = 1,
    LineString = 2,
    Polygon = 3,
};

const char* to_string(GeomType type);

// One MVT attribute value. The wire has seven alternatives; they collapse to
// four useful ones here, with the integer flavours (int64/uint64/sint64) all
// landing in Integer because nothing downstream cares which encoding was used.
using Value = std::variant<std::monostate, std::string, double, std::int64_t, bool>;

std::string valueToString(const Value& value);

struct Feature
{
    std::uint64_t id { 0 };
    bool hasId { false };
    GeomType type { GeomType::Unknown };

    // Each MoveTo begins a new ring. A LineString has one ring per part; a
    // Polygon has an exterior ring followed by its holes, then the next
    // polygon's exterior, and so on -- which is why winding is the only way to
    // tell where one polygon ends.
    std::vector<std::vector<Point>> rings;

    // Indices into the layer's keys and values, in pairs. Kept as indices
    // rather than resolved strings because a tile has thousands of features
    // over a handful of distinct values, and resolving eagerly would copy the
    // same string a thousand times.
    std::vector<std::uint32_t> tags;
};

struct Layer
{
    std::string name;
    std::uint32_t version { 1 };
    // Per layer, default 4096. See the header comment.
    std::uint32_t extent { 4096 };

    std::vector<std::string> keys;
    std::vector<Value> values;
    std::vector<Feature> features;

    // Resolve one of a feature's attributes. Returns nothing when the feature
    // does not carry that key.
    std::optional<Value> attribute(const Feature& feature, std::string_view key) const;

    // The common case by a wide margin: OpenMapTiles puts the thing a style
    // switches on in `class`.
    std::string attributeText(const Feature& feature, std::string_view key) const;
};

struct Tile
{
    std::vector<Layer> layers;

    const Layer* layer(std::string_view name) const;
};

// Twice the signed area of a ring, in tile units. Sign is what matters:
// positive is an exterior ring, negative is a hole. Returned undoubled-and-
// unhalved because the halving is pure loss for a sign test, and because the
// doubled value stays an exact integer.
std::int64_t signedArea2(const std::vector<Point>& ring);

inline bool isExteriorRing(const std::vector<Point>& ring)
{
    return signedArea2(ring) > 0;
}

} // namespace mvt

#endif // MVT_TILE_H
