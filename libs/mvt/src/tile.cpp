// SPDX-License-Identifier: GPL-3.0-or-later
#include "mvt/tile.h"

#include <algorithm>

namespace mvt
{

const char* to_string(GeomType type)
{
    switch (type)
    {
        case GeomType::Unknown:
            return "unknown";
        case GeomType::Point:
            return "point";
        case GeomType::LineString:
            return "linestring";
        case GeomType::Polygon:
            return "polygon";
    }

    return "unrecognised";
}

std::string valueToString(const Value& value)
{
    if (const auto* text = std::get_if<std::string>(&value))
    {
        return *text;
    }
    if (const auto* number = std::get_if<double>(&value))
    {
        return std::to_string(*number);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value))
    {
        return std::to_string(*integer);
    }
    if (const auto* flag = std::get_if<bool>(&value))
    {
        return *flag ? "true" : "false";
    }
    return {};
}

std::optional<Value> Layer::attribute(const Feature& feature, std::string_view key) const
{
    // Find the key's index once, then look for it among the feature's tags. The
    // other way round -- resolving every tag to a string and comparing -- is
    // what a naive implementation does, and it is a string compare per tag per
    // feature over tens of thousands of features per tile.
    const auto found = std::find(keys.begin(), keys.end(), key);
    if (found == keys.end())
    {
        return std::nullopt;
    }
    const auto wanted = static_cast<std::uint32_t>(std::distance(keys.begin(), found));

    // Tags are (key index, value index) pairs. A trailing odd element is
    // refused at decode time, so stepping by two is safe here.
    for (std::size_t i = 0; i + 1 < feature.tags.size(); i += 2)
    {
        if (feature.tags[i] != wanted)
        {
            continue;
        }

        const std::uint32_t valueIndex = feature.tags[i + 1];
        if (valueIndex >= values.size())
        {
            // Also refused at decode time; belt and braces, because the
            // alternative here is an out-of-bounds read.
            return std::nullopt;
        }
        return values[valueIndex];
    }

    return std::nullopt;
}

std::string Layer::attributeText(const Feature& feature, std::string_view key) const
{
    const auto value = attribute(feature, key);
    return value.has_value() ? valueToString(*value) : std::string {};
}

const Layer* Tile::layer(std::string_view name) const
{
    const auto found =
        std::find_if(layers.begin(), layers.end(), [name](const Layer& l) { return l.name == name; });
    return (found == layers.end()) ? nullptr : &*found;
}

std::int64_t signedArea2(const std::vector<Point>& ring)
{
    if (ring.size() < 3)
    {
        return 0;
    }

    // The shoelace formula, in exact integer arithmetic. Tile coordinates are
    // int32 and a ring can have thousands of points, so the products are
    // accumulated in int64: at 4096 extent the terms are small, but a tile
    // buffer legitimately puts coordinates well outside that and doubles would
    // start losing the low bits of a sum whose SIGN is the entire answer.
    std::int64_t total = 0;
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        const Point& current = ring[i];
        const Point& next = ring[(i + 1) % ring.size()];
        total += (static_cast<std::int64_t>(current.x) * next.y) -
                 (static_cast<std::int64_t>(next.x) * current.y);
    }
    return total;
}

} // namespace mvt
