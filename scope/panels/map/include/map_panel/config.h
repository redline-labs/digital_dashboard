#ifndef SCOPE_MAP_PANEL_CONFIG_H_
#define SCOPE_MAP_PANEL_CONFIG_H_

#include "config_codec/config_limits.h"
#include "helpers/color.h"
#include "map_render/style.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// How the trail is coloured along its length.
//
// A RAMP IS THE POINT OF THE FEATURE. A plot answers "what was the speed at
// this moment"; a map coloured by speed answers "where on this lap was I slow",
// which is the question a plot is worst at and the reason to want a map beside
// one.
REFLECT_ENUM(map_color_ramp_t,
    // Perceptually uniform, colour-blind safe, and dark at the low end -- which
    // matters on a dark basemap, where a ramp that bottoms out light makes slow
    // sections shout.
    viridis,

    // Higher contrast at both ends. Better for spotting the extremes of a
    // signal, worse for judging the middle.
    turbo,

    // No hue at all. For when the map underneath is already carrying colour --
    // a track overlay, a highlighted route -- and a second colour axis would
    // fight it.
    gray
)

// One signal the panel reads.
//
// The tree's binding triple, and DELIBERATELY NOT table_row_t or
// signal_binding_t: a plot's trace carries `color`, `right_axis` and `display`
// and a table's row carries `format`, `units` and `decimals`, none of which a
// map coordinate has any meaning for. Sharing one of those structs would put
// fields in this panel's reflected config that the YAML encoder writes, the
// agent interface advertises as settable and the panel silently ignores.
REFLECT_STRUCT(map_binding_t,
    (std::string, zenoh_key, "",
        "Zenoh Key", "Zenoh topic key to subscribe to. Empty means this role is unbound"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::GsofLatLongHeight,
        "Schema Type", "Data schema the topic is published with"),
    (std::string, value_expression, "",
        "Value Expression", "Arithmetic over the schema's numeric fields, e.g. 'latitudeDeg'")
)

// Where the vehicle went, under the shared clock.
//
// THE PANEL IS NOT dashboard/widgets/map. That one follows a live vehicle and
// pulls its tiles from nodes/map_server; this one draws a whole retention
// window of history at once, puts a marker on the shared cursor, and reads its
// tiles straight off disk. The renderer underneath is shared (libs/map_render);
// the camera, the tile selection, the paint driver and this config are not.
//
// NO position_zenoh_key/latitude_expression pair as the widget has, because a
// panel is BOUND rather than configured: the browser and the drag hand it
// candidates, and `latitude`/`longitude`/`color_by` are where they land.
REFLECT_STRUCT(MapPanelConfig_t,
    (std::string, title, "Map",
        "Title", "Shown on the panel's title bar"),

    // A NAME, NEVER A PATH. The path is in the per-user settings file
    // (scope/settings.h), and that split is what keeps a workspace openable on
    // someone else's machine.
    (std::string, tileset, "",
        "Tileset", "Name of a tileset from Settings. Empty draws no basemap"),
    (std::vector<std::string>, overlay_tilesets, {},
        "Overlay Tilesets", "Extra tilesets drawn over the base one, e.g. 'tracks'. Each is "
                            "an independent archive with its own zoom range"),

    // uint16_t, not uint8_t, and that is not arbitrary: yaml-cpp treats
    // `unsigned char` as a CHARACTER type, so a zoom of 14 is written as the
    // unprintable byte 0x0E and read back as a bad conversion that throws out of
    // the decoder and takes the whole WORKSPACE with it. Nothing in the type
    // says so and it fails at load time rather than at compile time.
    (uint16_t, min_zoom, 0,
        "Minimum Zoom", "Shallowest the camera may go"),
    (uint16_t, max_zoom, 17,
        "Maximum Zoom", "Deepest the camera may go. Past what the archive holds this "
                        "magnifies its deepest tiles rather than blanking"),

    (double, center_latitude, 33.6865966,
        "Center Latitude", "Degrees north. Where the camera sits before any position arrives"),
    (double, center_longitude, -117.8557874,
        "Center Longitude", "Degrees east"),
    (double, zoom, 14.0,
        "Zoom", "0 is the whole world, each step doubles. 14 is street level"),

    // ON by default, which is the opposite of the dashboard widget's choice and
    // for the opposite reason: a dashboard is a surface people brace a hand
    // against on a bad road, and a review tool is a window someone is deliberately
    // exploring. A map you cannot pan is not much use for reviewing a drive.
    (bool, interactive, true,
        "Interactive", "Drag to pan and use the wheel to zoom"),

    // Keeps the camera on the MARKER, which is the shared cursor's position --
    // not on a live vehicle, because there may not be one. Scrubbing then walks
    // the map along the drive.
    (bool, follow_cursor, true,
        "Follow Cursor", "Keep the camera on the marker as the shared cursor moves"),

    // THE REASON A MAP BELONGS IN A REVIEW TOOL, and the one direction the
    // dashboard widget has no equivalent of: clicking the drawn track moves the
    // SHARED cursor, so every plot, table and video frame jumps to that corner.
    (bool, click_seeks, true,
        "Click Seeks", "Clicking the track moves the shared time cursor to that point, so "
                       "every other panel jumps with it"),
    (double, click_radius_px, 12.0,
        "Click Radius", "How close a click has to land to count as on the track. Outside "
                        "it, a drag pans the map instead"),

    (map_binding_t, latitude, {},
        "Latitude", "Signal giving degrees north"),
    (map_binding_t, longitude, {},
        "Longitude", "Signal giving degrees east. Must be on the same topic as latitude -- "
                     "they pair by the timestamp their shared message carries"),
    (map_binding_t, color_by, {},
        "Colour By", "Optional third signal driving a colour ramp along the trail"),

    (map_color_ramp_t, color_ramp, map_color_ramp_t::viridis,
        "Colour Ramp", "Which ramp the trail is coloured with"),
    (bool, color_autoscale, true,
        "Colour Autoscale", "Fit the ramp to the range actually present. Off uses the "
                            "limits below"),
    (double, color_min, 0.0,
        "Colour Minimum", "Value at the bottom of the ramp when autoscale is off"),
    (double, color_max, 100.0,
        "Colour Maximum", "Value at the top of the ramp when autoscale is off"),
    (bool, show_color_legend, true,
        "Show Colour Legend", "Draw the ramp and its range in a corner"),

    (helpers::Color, track_color, "#FF3B30",
        "Track Color", "Colour of the trail when nothing drives the ramp"),
    (double, track_width, 3.0,
        "Track Width", "Line width of the trail in pixels"),

    // THE BAND THAT TIES THE MAP TO THE TIME BASE. The whole retention window is
    // drawn dim; the stretch inside the view is drawn solid and wider. Without
    // it the map is a picture beside the plots rather than a view of the same
    // window, and there is nothing on screen relating the two.
    (double, track_opacity, 0.35,
        "Track Opacity", "How faint the part of the trail outside the current view is"),
    (double, view_track_width, 4.5,
        "View Track Width", "Line width of the stretch inside the current view, which is "
                            "drawn solid while the rest is dimmed"),

    (helpers::Color, marker_color, "#FFFFFF",
        "Marker Color", "Colour of the marker at the shared cursor"),
    (uint16_t, marker_size, 7,
        "Marker Size", "Radius of the marker in pixels"),
    (helpers::Color, marker_outline_color, "#101216",
        "Marker Outline", "Ring around the marker. It is what keeps the marker legible "
                          "over a trail of a similar colour"),

    (MapStyle_t, style, {},
        "Style", "Colours and widths for the map itself"),

    // Without it a tileset that is not configured, an archive that will not
    // open, a hole in coverage and an unbound position all look like the same
    // empty panel.
    (bool, show_status, true,
        "Show Status", "Draw a line of text explaining an empty map")
)

// Clamps rather than refuses, like every other config in this tree: a workspace
// with one nonsense number should still open.
inline std::vector<std::string> validate(MapPanelConfig_t& cfg)
{
    std::vector<std::string> notes;

    // The projection's own limits. Latitude past +/-85.05 is outside Web
    // Mercator entirely and projects to infinity, which paints as nothing.
    config_codec::limits::clampInto<double>(cfg.center_latitude, -85.05112878, 85.05112878,
                                            "center_latitude", notes);
    config_codec::limits::clampInto<double>(cfg.center_longitude, -180.0, 180.0,
                                            "center_longitude", notes);
    config_codec::limits::clampInto<double>(cfg.zoom, 0.0, 22.0, "zoom", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.min_zoom, 0u, 22u, "min_zoom", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.max_zoom, 0u, 22u, "max_zoom", notes);

    // An inverted range would refuse every zoom -- clamp(z, 17, 0) has no answer
    // that satisfies both ends -- so the wheel would do nothing and nothing
    // would say why. Swapping is the only reading that leaves it usable.
    config_codec::limits::orderRange(cfg.min_zoom, cfg.max_zoom, "the zoom range", notes);

    config_codec::limits::clampInto<uint16_t>(cfg.marker_size, 2u, 64u, "marker_size", notes);
    config_codec::limits::clampInto<double>(cfg.track_width, 0.5, 20.0, "track_width", notes);
    config_codec::limits::clampInto<double>(cfg.view_track_width, 0.5, 24.0, "view_track_width",
                                            notes);
    config_codec::limits::clampInto<double>(cfg.track_opacity, 0.0, 1.0, "track_opacity", notes);

    // Below about four pixels the track is unhittable and clicking it looks
    // broken; much above thirty and a click meant for the map seeks instead.
    config_codec::limits::clampInto<double>(cfg.click_radius_px, 4.0, 30.0, "click_radius_px",
                                            notes);

    // An inverted or empty colour range divides by zero when a value is mapped
    // onto it, and every point comes out the same colour -- which reads as a
    // ramp that is not working rather than as a range that is wrong.
    config_codec::limits::orderRange(cfg.color_min, cfg.color_max, "the colour range", notes);
    if (!(cfg.color_max > cfg.color_min))
    {
        cfg.color_max = cfg.color_min + 1.0;
        notes.emplace_back("colour range was empty; widened it to 1 unit");
    }

    const std::vector<std::string> style_notes = validate(cfg.style);
    notes.insert(notes.end(), style_notes.begin(), style_notes.end());

    return notes;
}

#endif  // SCOPE_MAP_PANEL_CONFIG_H_
