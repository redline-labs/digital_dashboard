// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_build/track_source.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include "road_graph/geometry.h"

namespace map_build::track
{

namespace
{

using nlohmann::json;

// A number, whatever the file chose to call it.
//
// The source is inconsistent about this and it matters: `length_m` is an
// integer, `gatewidth_m` is a float, and `resolution_x` is a STRING holding a
// float. A reader that takes only one of those silently gets zero for the
// others, and zero is the value that means "the file did not say".
double numberOr(const json& object, const char* key, double fallback)
{
    const auto found = object.find(key);
    if (found == object.end())
    {
        return fallback;
    }
    if (found->is_number())
    {
        return found->get<double>();
    }
    if (found->is_string())
    {
        try
        {
            return std::stod(found->get<std::string>());
        }
        catch (const std::exception&)
        {
            return fallback;
        }
    }
    return fallback;
}

std::string stringOr(const json& object, const char* key, const std::string& fallback)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string())
    {
        return fallback;
    }
    return found->get<std::string>();
}

bool boolOr(const json& object, const char* key, bool fallback)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_boolean())
    {
        return fallback;
    }
    return found->get<bool>();
}

// GeoJSON is [longitude, latitude]. This tree is lat/lon, everywhere.
//
// The swap gets its own function because of how it fails: a track with its
// coordinates the wrong way round still parses, still tiles, and still draws --
// somewhere in the Indian Ocean, at a latitude that does not exist for half the
// corpus. Doing it in one place means one place to get it right.
bool appendPoint(Ring& ring, const json& pair)
{
    if (!pair.is_array() || pair.size() < 2 || !pair[0].is_number() || !pair[1].is_number())
    {
        return false;
    }
    ring.push_back(road_graph::fromDegrees(pair[1].get<double>()));
    ring.push_back(road_graph::fromDegrees(pair[0].get<double>()));
    return true;
}

// The outline's coordinates, whichever of the two geometry types carries them.
// A Polygon's first ring is the outline; the source never writes a second one
// (every closed file has exactly one entry in `coordinates`, which is the whole
// reason the seam has to be found rather than read off).
bool readOutline(const json& geometry, Ring& out)
{
    const std::string type = stringOr(geometry, "type", "");
    const auto coordinates = geometry.find("coordinates");
    if (coordinates == geometry.end() || !coordinates->is_array())
    {
        return false;
    }

    const json* points = nullptr;
    if (type == "Polygon")
    {
        if (coordinates->empty() || !(*coordinates)[0].is_array())
        {
            return false;
        }
        points = &(*coordinates)[0];
    }
    else if (type == "LineString")
    {
        points = &(*coordinates);
    }
    else
    {
        return false;
    }

    for (const auto& pair : *points)
    {
        if (!appendPoint(out, pair))
        {
            return false;
        }
    }

    // The closing repeat, dropped. Every ring in this tree leaves the closing
    // edge implied, and carrying the duplicate through would put a zero-length
    // segment at the seam -- which is precisely where the derivation is looking
    // for the smallest distance it can find.
    const std::size_t count = pointCount(out);
    if (count >= 2 && out[0] == out[2 * (count - 1)] && out[1] == out[2 * (count - 1) + 1])
    {
        out.resize(out.size() - 2);
    }
    return pointCount(out) >= 3;
}

} // namespace

const char* to_string(LoadStatus status)
{
    switch (status)
    {
        case LoadStatus::Ok:
            return "ok";
        case LoadStatus::Unreadable:
            return "unreadable";
        case LoadStatus::NotJson:
            return "not-json";
        case LoadStatus::NotFeatureCollection:
            return "not-feature-collection";
        case LoadStatus::NoOutline:
            return "no-outline";
        case LoadStatus::SeveralOutlines:
            return "several-outlines";
        case LoadStatus::BadGeometry:
            return "bad-geometry";
    }
    return "unknown";
}

const char* to_string(GatePointStatus status)
{
    switch (status)
    {
        case GatePointStatus::Present:
            return "present";
        case GatePointStatus::Absent:
            return "absent";
        case GatePointStatus::Several:
            return "several";
    }
    return "unknown";
}

LoadResult loadSourceFile(const std::filesystem::path& path)
{
    LoadResult result;
    result.file.id = path.stem().string();

    std::ifstream stream(path);
    if (!stream)
    {
        result.status = LoadStatus::Unreadable;
        result.error = "cannot open";
        return result;
    }

    json document;
    try
    {
        stream >> document;
    }
    catch (const std::exception& error)
    {
        result.status = LoadStatus::NotJson;
        result.error = error.what();
        return result;
    }

    if (stringOr(document, "type", "") != "FeatureCollection")
    {
        result.status = LoadStatus::NotFeatureCollection;
        result.error = "type is not FeatureCollection";
        return result;
    }

    const auto features = document.find("features");
    if (features == document.end() || !features->is_array())
    {
        result.status = LoadStatus::NoOutline;
        result.error = "no features array";
        return result;
    }

    std::uint32_t outlines = 0;
    std::uint32_t points = 0;

    for (const auto& feature : *features)
    {
        const auto geometry = feature.find("geometry");
        if (geometry == feature.end() || !geometry->is_object())
        {
            continue;
        }
        const std::string type = stringOr(*geometry, "type", "");

        const json empty = json::object();
        const auto propertiesIt = feature.find("properties");
        const json& properties =
            (propertiesIt != feature.end() && propertiesIt->is_object()) ? *propertiesIt : empty;

        if (type == "Point")
        {
            ++points;
            if (points > 1)
            {
                continue;
            }
            const auto coordinates = geometry->find("coordinates");
            Ring single;
            if (coordinates == geometry->end() || !appendPoint(single, *coordinates))
            {
                result.status = LoadStatus::BadGeometry;
                result.error = "start/finish point has no usable coordinates";
                return result;
            }
            result.file.gateLat = single[0];
            result.file.gateLon = single[1];
            result.file.circuit = stringOr(properties, "circuit", "");
            result.file.publishedLengthM = numberOr(properties, "length_m", 0.0);
            result.file.gateWidthM = numberOr(properties, "gatewidth_m", 0.0);
            result.file.combo = boolOr(properties, "combo", false);
            continue;
        }

        if (type != "Polygon" && type != "LineString")
        {
            continue;
        }

        ++outlines;
        if (outlines > 1)
        {
            result.status = LoadStatus::SeveralOutlines;
            result.error = "more than one outline feature";
            return result;
        }

        if (!readOutline(*geometry, result.file.outline))
        {
            result.status = LoadStatus::BadGeometry;
            result.error = "outline coordinates are not a usable ring";
            return result;
        }
        result.file.name = stringOr(properties, "name", "");
        result.file.closed = boolOr(properties, "closed", type == "Polygon");
        result.file.degenerate = boolOr(properties, "degenerate", false);
    }

    if (outlines == 0)
    {
        result.status = LoadStatus::NoOutline;
        result.error = "no Polygon or LineString feature";
        return result;
    }

    result.file.gatePoint = points == 0   ? GatePointStatus::Absent
                            : points == 1 ? GatePointStatus::Present
                                          : GatePointStatus::Several;
    if (result.file.gatePoint == GatePointStatus::Several)
    {
        // Everything read from the first point is withdrawn. A published length
        // taken from one of several candidates would sit in the QA gate looking
        // authoritative.
        result.file.publishedLengthM = 0.0;
        result.file.gateWidthM = 0.0;
    }

    // A file with no `name` falls back to its stem. Ten do.
    if (result.file.name.empty())
    {
        result.file.name = result.file.circuit.empty() ? result.file.id : result.file.circuit;
    }
    if (result.file.circuit.empty())
    {
        result.file.circuit = result.file.name;
    }

    result.status = LoadStatus::Ok;
    return result;
}

std::vector<std::filesystem::path> listSourceFiles(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (!entry.is_regular_file(ec))
        {
            continue;
        }
        if (entry.path().extension() == ".geojson")
        {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace map_build::track
