// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `metadata` table of an .mbtiles archive, parsed.
//
// The spec (mbtiles 1.3) requires only `name` and `format`; everything else is
// optional, and real archives leave most of it out. So every field here has a
// "was it there at all" answer rather than a plausible default -- absent bounds
// are not the whole world, and an absent center is not null island. A client
// that cannot tell the difference will happily fly the camera to 0,0.
#ifndef MBTILES_METADATA_H
#define MBTILES_METADATA_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mbtiles
{

struct Metadata
{
    // Required by the spec. Empty if the archive omitted them anyway.
    std::string name;
    std::string format;

    std::string version;
    std::string description;
    std::string attribution;
    std::string type;

    // From the metadata table when present, otherwise from
    // SELECT MIN/MAX(zoom_level) over the tiles -- which is slower but is what
    // makes an archive without them usable rather than unusable.
    std::uint8_t minzoom { 0 };
    std::uint8_t maxzoom { 0 };

    // [west, south, east, north] and [lon, lat, zoom]. Empty when the archive
    // did not say, or said something that did not parse as the right number of
    // numbers -- a three-element `bounds` is dropped rather than half-read.
    std::vector<double> bounds;
    std::vector<double> center;

    // The `json` column verbatim: the vector_layers list, mostly. Not parsed
    // here -- it is passed through into the TileJSON, and this library has no
    // opinion about its contents.
    std::string json;

    // Anything else the table held. Kept because the spec explicitly allows
    // arbitrary rows and dropping them would make this a lossy read.
    std::vector<std::pair<std::string, std::string>> extra;
};

// Parse a "west,south,east,north" style list.
//
// Returns nothing unless it parsed as exactly `expected` finite numbers. Half a
// bounding box is worse than none: it renders, it just renders somewhere else.
std::optional<std::vector<double>> parseNumberList(const std::string& value, std::size_t expected);

} // namespace mbtiles

#endif // MBTILES_METADATA_H
