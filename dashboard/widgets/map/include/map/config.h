// SPDX-License-Identifier: GPL-3.0-or-later
//
// The offline map widget's configuration.
//
// Everything the map draws comes from nodes/map_server over zenoh. There is no
// URL here and no style document: the tiles are asked for by tileset name and
// z/x/y, and the look is the nested MapStyle_t, which the editor's properties
// panel edits like any other field. See docs/map.md.
//
// The default centre is Irvine, CA, because the archive on the bench covers
// Southern California and a map widget that opens on null island looks broken
// in exactly the way a misconfigured one does.
#ifndef MAP_CONFIG_H
#define MAP_CONFIG_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "config_codec/config_limits.h"
#include "helpers/color.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

#include "map/style.h"

REFLECT_STRUCT(MapConfig_t,
    (std::string, tileset, "socal",
        "Tileset", "Tileset name as configured in nodes/map_server"),
    (std::string, tile_zenoh_key, "map/tile",
        "Tile Zenoh Key", "Service key map_server answers tile requests on"),
    (uint16_t, request_timeout_ms, 4000,
        "Request Timeout (ms)", "How long to wait for a tile before giving up on it"),
    // uint16_t, not uint8_t, and that is not arbitrary. yaml-cpp treats
    // `unsigned char` as a CHARACTER type: a zoom of 14 is written as the
    // unprintable byte 0x0E and read back as a bad conversion, which throws out
    // of the YAML decoder and takes the whole layout with it. Nothing in the
    // type says so, and it fails at load time rather than at compile time. No
    // reflected config in this tree should use an 8-bit integer.
    (uint16_t, min_zoom, 0,
        "Minimum Zoom", "Shallowest tile level the archive has"),
    (uint16_t, max_zoom, 14,
        "Maximum Zoom", "Deepest tile level the archive has. Zooming past this magnifies rather than blanking"),

    (double, center_latitude, 33.6865966,
        "Center Latitude", "Degrees north. Used until a position arrives, and whenever Follow Vehicle is off"),
    (double, center_longitude, -117.8557874,
        "Center Longitude", "Degrees east"),
    (double, zoom, 13.0,
        "Zoom", "0 is the whole world, each step doubles. 14 is street level"),
    (double, bearing, 0.0,
        "Bearing", "Map rotation in degrees clockwise from north"),

    (bool, follow_vehicle, true,
        "Follow Vehicle", "Keep the camera centred on the vehicle position as it arrives"),
    (bool, rotate_with_heading, false,
        "Rotate With Heading", "Turn the map so the vehicle's heading points up. Needs a heading expression"),
    (bool, show_track, true,
        "Show Track", "Draw a trail behind the vehicle"),
    (uint16_t, track_points, 600,
        "Track Points", "How many positions the trail keeps. 0 disables it"),

    (std::string, position_zenoh_key, "",
        "Position Zenoh Key", "Topic carrying the vehicle position, e.g. nodes/bd992/position"),
    (pub_sub::schema_type_t, position_schema_type, pub_sub::schema_type_t::GsofLatLongHeight,
        "Position Schema Type", "Schema of the position topic"),
    (std::string, latitude_expression, "",
        "Latitude Expression", "Expression yielding degrees north, e.g. latitudeDeg"),
    (std::string, longitude_expression, "",
        "Longitude Expression", "Expression yielding degrees east, e.g. longitudeDeg"),
    (std::string, heading_expression, "",
        "Heading Expression", "Expression yielding degrees clockwise from north. Optional"),

    (helpers::Color, marker_color, "#FF3B30",
        "Marker Color", "Colour of the vehicle marker and its trail"),
    (uint16_t, marker_size, 9,
        "Marker Size", "Radius of the vehicle marker in pixels"),
    (helpers::Color, marker_outline_color, "#FFFFFF",
        "Marker Outline", "Ring around the vehicle marker. It is what keeps the marker legible over a road of a similar colour"),
    (double, track_width, 3.0,
        "Track Width", "Line width of the trail behind the vehicle, in pixels"),
    (double, track_opacity, 0.7,
        "Track Opacity", "0 is invisible, 1 is as solid as the marker"),

    (MapStyle_t, style, {},
        "Style", "Colours and widths for the map itself"),

    (bool, show_status, true,
        "Show Status", "Draw a line of text when no tiles are arriving. Without it, a map_server that is not running looks like an empty map")
)

// Clamps rather than refuses, like every other widget's config: a layout with
// one nonsense number should still draw a map on the way to a track day.
//
// The ranges are the projection's own. Latitude past +/-85.05 is outside Web
// Mercator entirely and projects to infinity, which paints as nothing at all.
inline std::vector<std::string> validate(MapConfig_t& config)
{
    std::vector<std::string> notes;

    config_codec::limits::clampInto<double>(config.center_latitude, -85.05112878, 85.05112878,
                                            "center_latitude", notes);
    config_codec::limits::clampInto<double>(config.center_longitude, -180.0, 180.0,
                                            "center_longitude", notes);
    config_codec::limits::clampInto<double>(config.zoom, 0.0, 22.0, "zoom", notes);
    config_codec::limits::clampInto<uint16_t>(config.min_zoom, 0u, 22u, "min_zoom", notes);
    config_codec::limits::clampInto<uint16_t>(config.max_zoom, 0u, 22u, "max_zoom", notes);
    config_codec::limits::clampInto<uint16_t>(config.marker_size, 2u, 64u, "marker_size", notes);
    config_codec::limits::clampInto<double>(config.track_width, 0.5, 20.0, "track_width", notes);
    config_codec::limits::clampInto<double>(config.track_opacity, 0.0, 1.0, "track_opacity", notes);
    config_codec::limits::clampInto<uint16_t>(config.request_timeout_ms, 100u, 30000u,
                                              "request_timeout_ms", notes);

    // An inverted zoom range would make every tile request fall outside it and
    // the map would be permanently blank. Swapping is the only reading that
    // leaves it usable.
    config_codec::limits::orderRange(config.min_zoom, config.max_zoom, "the zoom range", notes);

    // Bearing wraps rather than clamping: 350 and -10 are the same heading, and
    // clamping would turn a legitimate north-north-west into due north.
    if (config.bearing < 0.0 || config.bearing >= 360.0)
    {
        config.bearing = config.bearing - (360.0 * std::floor(config.bearing / 360.0));
    }

    const std::vector<std::string> styleNotes = validate(config.style);
    notes.insert(notes.end(), styleNotes.begin(), styleNotes.end());

    return notes;
}

#endif // MAP_CONFIG_H
