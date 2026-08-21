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
    // Drawn OVER the base tileset, in this order, from their own archives.
    //
    // A separate archive rather than more layers in one, because the two are
    // updated on their own schedules from their own sources: the race-track
    // layer is global and rebuilt whenever new track maps arrive, the basemap
    // is regional and rebuilt from an OSM extract, and neither may force the
    // other. Each overlay keeps its own zoom range, cache and backoff, so a
    // missing overlay archive is distinguishable from a hole in coverage.
    //
    // Costs one extra query batch per viewport per overlay. That is cheap for a
    // sparse layer: nearly every tile comes back notFound, and an absent tile
    // is cached as absent and never asked for again.
    (std::vector<std::string>, overlay_tilesets, {},
        "Overlay Tilesets", "Extra tilesets drawn over the base one, e.g. 'tracks'. Each is an independent archive with its own zoom range"),
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
    //
    // The CAMERA's limits, not the archive's. Which tile level is drawn is not
    // configured at all any more: map_server reports what its archive actually
    // holds on every reply, and the widget clamps its requests to that. So
    // these two say how far a layout lets the user zoom, and nothing else --
    // asking for more than the archive has magnifies the deepest tiles it does
    // have, which for vector tiles stays sharp and simply shows less.
    //
    // 17 rather than 22, because past about three levels beyond the archive
    // there is so little left in frame that it stops being a map. Measured
    // against the SoCal archive; a deeper archive wants a higher number.
    (uint16_t, min_zoom, 0,
        "Minimum Zoom", "Shallowest the camera may go. Not the archive's range -- that comes from the server"),
    (uint16_t, max_zoom, 17,
        "Maximum Zoom", "Deepest the camera may go. Past what the archive holds this magnifies its deepest tiles rather than blanking"),

    (double, center_latitude, 33.6865966,
        "Center Latitude", "Degrees north. Used until a position arrives, and whenever Follow Vehicle is off"),
    (double, center_longitude, -117.8557874,
        "Center Longitude", "Degrees east"),
    (double, zoom, 13.0,
        "Zoom", "0 is the whole world, each step doubles. 14 is street level"),
    (double, bearing, 0.0,
        "Bearing", "Map rotation in degrees clockwise from north"),

    // OFF by default, and that is not timidity. A dashboard is a surface people
    // brace a hand against on a bad road, and a map that pans out of position
    // when they do is worse than one that never moves. A layout that wants it
    // asks for it.
    (bool, interactive, false,
        "Interactive", "Allow dragging to pan and the wheel to zoom. A recentre button appears once the camera has been moved"),

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

    (std::string, highlight_zenoh_key, "",
        "Highlight Zenoh Key", "Topic carrying the matcher's horizon (MapHorizon), e.g. nodes/map_match/horizon. The matched road ahead lights up in the highlight colour. Way ids only survive in tiles at z13 and deeper, so the highlight quietly disappears when zoomed shallower. Empty disables it"),
    (helpers::Color, highlight_color, "#00E5FFB0",
        "Highlight Color", "Colour the matched road is recoloured with"),
    (double, highlight_extra_width, 2.0,
        "Highlight Extra Width", "Extra half-width in pixels beyond the road's own, so the highlight reads as a casing rather than vanishing into the road it recolours"),

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

    (uint16_t, tile_fade_ms, 150,
        "Tile Fade (ms)", "How long a newly arrived tile takes to fade in. The old zoom level stays underneath for the duration, so tiles blend in rather than popping. 0 disables the fade"),

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
    config_codec::limits::clampInto<double>(config.highlight_extra_width, 0.0, 20.0,
                                            "highlight_extra_width", notes);
    config_codec::limits::clampInto<uint16_t>(config.tile_fade_ms, 0u, 1000u, "tile_fade_ms",
                                              notes);

    // An inverted camera range would refuse every zoom -- clamp(z, 17, 0) has
    // no answer that satisfies both ends -- so the wheel would do nothing and
    // nothing would say why. Swapping is the only reading that leaves it
    // usable. It no longer affects which TILES are asked for: that range comes
    // from the server.
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
