// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the map looks.
//
// A reflected struct rather than a GL style document, and that is a deliberate
// trade. A GL style is a small programming language -- filters, expressions,
// data-driven property functions, zoom interpolation -- and implementing it
// faithfully is a larger job than the renderer it configures. What a dashboard
// needs is a colour, a width and a zoom threshold per kind of thing.
//
// What that buys: this appears in the editor's properties panel like every
// other widget's config, round-trips through YAML, and is settable over
// widget.set_config -- none of which a style.json would have been. The cost is
// that you cannot drop in someone else's style.
//
// The field names are OpenMapTiles layer and class names, because that is the
// schema the archive is built to. See docs/map.md.
//
// WHAT IS NOT HERE, and cannot be: the archive decides which features exist at
// which zoom, and no setting can add back what tilemaker dropped. Lowering
// `detail.building` below the archive's own z13 draws nothing extra. The
// thresholds here can only ever be stricter than the data; docs/map.md has the
// measured table.
#ifndef MAP_STYLE_H
#define MAP_STYLE_H

#include <cstdint>
#include <string>
#include <vector>

#include "config_codec/config_limits.h"
#include "helpers/color.h"
#include "reflection/reflection.h"

// Half-widths in screen pixels at full scale, i.e. what the layer measures at
// z14 before `road_width_scale` and the zoom taper are applied. Half, not
// whole, because a line is expanded either side of its centre.
//
// Zero hides a layer outright, which is the cheapest way to drop something
// without touching the archive.
REFLECT_STRUCT(MapWidths_t,
    (double, motorway, 3.75,
        "Motorway", "Half-width of the motorway fill, in pixels at zoom 14"),
    (double, motorway_casing, 5.5,
        "Motorway Casing", "Half-width of the darker outline under a motorway. Must exceed the fill or the casing never shows"),
    (double, road_primary, 3.5,
        "Primary Road", "Primary roads and trunk routes"),
    (double, road_major, 2.5,
        "Major Road", "Secondary and tertiary roads"),
    (double, road_minor, 1.5,
        "Minor Road", "Residential streets, service roads and tracks"),
    (double, rail, 0.9,
        "Rail", "Railway lines"),
    // A runway is 45 m of concrete against a 23 m taxiway, and at z14 a metre
    // is about a third of a pixel -- so these are roughly to scale rather than
    // picked to look right, and they hold their ratio as the scale changes.
    (double, aeroway_runway, 7.0,
        "Runway", "Half-width of a runway. Wide enough to read as the landmark that identifies an airport"),
    (double, aeroway_taxiway, 2.0,
        "Taxiway", "Half-width of a taxiway, apron edge or anything else in the aeroway layer that is not a runway"),
    // ADDED to the road's own half-width rather than replacing it, so a
    // motorway bridge and a lane bridge get the same visual lip.
    (double, bridge_casing, 1.5,
        "Bridge Casing", "Extra half-width added around a bridge deck. 0 draws the deck with no outline, so it stops reading as a bridge"),
    (double, waterway, 1.25,
        "Waterway", "Rivers, streams and canals"),
    (double, boundary, 0.9,
        "Boundary", "State and country borders"),
    (double, racetrack_centre, 1.0,
        "Racetrack Centre Line", "The derived centre line of a race track")
)

inline std::vector<std::string> validate(MapWidths_t& widths)
{
    std::vector<std::string> notes;
    const auto clamp = [&notes](double& value, const char* name) {
        // Zero is legal and means "do not draw". The ceiling is what still
        // reads as a road rather than as a filled region at z14.
        config_codec::limits::clampInto<double>(value, 0.0, 40.0, name, notes);
    };
    clamp(widths.motorway, "widths.motorway");
    clamp(widths.motorway_casing, "widths.motorway_casing");
    clamp(widths.road_primary, "widths.road_primary");
    clamp(widths.road_major, "widths.road_major");
    clamp(widths.road_minor, "widths.road_minor");
    clamp(widths.rail, "widths.rail");
    clamp(widths.aeroway_runway, "widths.aeroway_runway");
    clamp(widths.aeroway_taxiway, "widths.aeroway_taxiway");
    clamp(widths.bridge_casing, "widths.bridge_casing");
    clamp(widths.waterway, "widths.waterway");
    clamp(widths.boundary, "widths.boundary");
    clamp(widths.racetrack_centre, "widths.racetrack_centre");
    return notes;
}

// The camera zoom below which a layer is not drawn at all.
//
// This is the clutter dial, and the only one that costs nothing to raise: the
// tiles are already on the wire, so a higher threshold saves DRAW CALLS, while
// a lower one saves nothing and may draw nothing at all -- see the header
// comment.
//
// Draw calls and not tessellation, and the difference is deliberate.
// `tessellate()` never consults `layerMinZoom`: a tile is turned into triangles
// once, on arrival, and reused at every zoom the camera later visits, so the
// zoom is not a thing it may look at. The cull is at draw time
// (gpu_renderer.cpp, the layer loop). Making this save tessellation too would
// mean passing the camera zoom into `tessellate()` and so making a tile's
// geometry depend on the zoom it happened to ARRIVE at -- which breaks the
// invariant the whole geometry cache rests on. Do not.
//
// What DOES skip tessellation is `show_*` and a width of zero, because neither
// depends on the camera. See `layerEnabled()` and `halfWidthFor()`.
//
// Defaults are tuned against the bench archive: buildings at z13 because that
// is the first zoom the archive carries them, minor roads at z12 because below
// it a city is a grey smear, motorways at z5 because they are the only useful
// thing left at continental zoom.
REFLECT_STRUCT(MapDetail_t,
    (uint16_t, landcover, 0,
        "Landcover From", "Lowest zoom that draws wood, forest, grass and farmland"),
    (uint16_t, landuse, 9,
        "Landuse From", "Lowest zoom that draws residential and built-up areas"),
    (uint16_t, park, 11,
        "Park From", "Lowest zoom that draws parks and nature reserves"),
    (uint16_t, water, 0,
        "Water From", "Lowest zoom that draws lakes and sea. The archive itself carries no water below z6"),
    (uint16_t, waterway, 8,
        "Waterway From", "Lowest zoom that draws rivers and streams as lines"),
    (uint16_t, building, 13,
        "Building From", "Lowest zoom that draws building footprints. The archive carries none below z13"),
    (uint16_t, road_minor, 12,
        "Minor Road From", "Lowest zoom that draws residential streets"),
    (uint16_t, road_major, 9,
        "Major Road From", "Lowest zoom that draws secondary and tertiary roads"),
    (uint16_t, road_primary, 7,
        "Primary Road From", "Lowest zoom that draws primary and trunk roads"),
    (uint16_t, motorway, 5,
        "Motorway From", "Lowest zoom that draws motorways and their casing"),
    (uint16_t, rail, 11,
        "Rail From", "Lowest zoom that draws railway lines"),
    // The archive's own floors are runway 10, apron/taxiway 13
    // (libs/map_rules/src/labels.cpp), and a threshold below those draws
    // nothing extra -- so these match rather than undercut them.
    (uint16_t, aeroway_surface, 11,
        "Aeroway Surface From", "Lowest zoom that draws aprons and aerodrome grounds. The archive's floor is z11 for aerodrome grounds, z13 for aprons"),
    (uint16_t, aeroway_runway, 10,
        "Runway From", "Lowest zoom that draws runways. The archive's own floor is z10"),
    (uint16_t, aeroway_taxiway, 13,
        "Taxiway From", "Lowest zoom that draws taxiways. The archive's own floor is z13"),
    // Street names are z14 information. Below that the roads themselves are a
    // grey smear (detail.road_minor is z12) and a name would be labelling
    // something the driver cannot see.
    (uint16_t, road_label, 14,
        "Road Label From", "Lowest zoom that draws street names"),
    // The archive only carries `brunnel` from z13 -- below the merge threshold
    // a per-road attribute would split features that should fold together --
    // so a lower threshold here draws nothing extra.
    (uint16_t, bridge, 13,
        "Bridge From", "Lowest zoom that draws bridges above the roads they cross. The archive carries no grade separation below z13"),
    (uint16_t, boundary, 0,
        "Boundary From", "Lowest zoom that draws state and country borders"),
    // A circuit is a kilometre across, so its surface is a few pixels before
    // z11 and its centre line is inside the surface until z12. Drawing either
    // sooner costs a tile fetch to paint a smudge.
    (uint16_t, racetrack_surface, 11,
        "Racetrack From", "Lowest zoom that draws the race track surface"),
    (uint16_t, racetrack_centre, 12,
        "Racetrack Centre From", "Lowest zoom that draws a race track's centre line")
)

inline std::vector<std::string> validate(MapDetail_t& detail)
{
    std::vector<std::string> notes;
    const auto clamp = [&notes](uint16_t& value, const char* name) {
        config_codec::limits::clampInto<uint16_t>(value, 0u, 22u, name, notes);
    };
    clamp(detail.landcover, "detail.landcover");
    clamp(detail.landuse, "detail.landuse");
    clamp(detail.park, "detail.park");
    clamp(detail.water, "detail.water");
    clamp(detail.waterway, "detail.waterway");
    clamp(detail.building, "detail.building");
    clamp(detail.road_minor, "detail.road_minor");
    clamp(detail.road_major, "detail.road_major");
    clamp(detail.road_primary, "detail.road_primary");
    clamp(detail.motorway, "detail.motorway");
    clamp(detail.rail, "detail.rail");
    clamp(detail.aeroway_surface, "detail.aeroway_surface");
    clamp(detail.aeroway_runway, "detail.aeroway_runway");
    clamp(detail.aeroway_taxiway, "detail.aeroway_taxiway");
    clamp(detail.road_label, "detail.road_label");
    clamp(detail.bridge, "detail.bridge");
    clamp(detail.boundary, "detail.boundary");
    clamp(detail.racetrack_surface, "detail.racetrack_surface");
    clamp(detail.racetrack_centre, "detail.racetrack_centre");
    return notes;
}

REFLECT_STRUCT(MapStyle_t,
    (helpers::Color, background, "#16181d",
        "Background", "Everything the map does not cover"),

    (helpers::Color, water, "#0f2231",
        "Water", "Lakes and sea"),
    (helpers::Color, waterway, "#0f2231",
        "Waterway", "Rivers, streams and canals. Defaults to the same colour as water"),
    (helpers::Color, landcover, "#1b2a20",
        "Landcover", "Wood, forest, grass and farmland"),
    (helpers::Color, landuse, "#1b1d23",
        "Landuse", "Residential and built-up areas"),
    (helpers::Color, park, "#1b2a20",
        "Park", "Parks and nature reserves"),
    (helpers::Color, building, "#242830",
        "Building", "Building footprints, drawn from zoom 13"),

    // Two colours and not three. A runway and a taxiway are the same concrete
    // and differ only in width; giving them separate colours invites a style
    // where the narrower line is the brighter one, which reads as the runway.
    (helpers::Color, aeroway_surface, "#20242c",
        "Aeroway Surface", "Aprons, aerodrome grounds and helipads, drawn as areas"),
    (helpers::Color, aeroway_line, "#39404b",
        "Aeroway Line", "Runways and taxiways. The runway is the wider of the two, not the brighter"),

    // A bridge keeps its own road colour -- it is the same road -- so the only
    // colour a bridge needs of its own is the casing that separates its deck
    // from whatever it crosses. Darker than the background, so it reads as a
    // shadow under the deck rather than as another road.
    (helpers::Color, bridge_casing, "#0d0f13",
        "Bridge Casing", "The outline around a bridge deck, which is what says the bridge is over and not under"),

    (helpers::Color, road_minor, "#2b3038",
        "Minor Road", "Residential streets, service roads and tracks"),
    (helpers::Color, road_major, "#3a414c",
        "Major Road", "Secondary and tertiary roads"),
    (helpers::Color, road_primary, "#59616f",
        "Primary Road", "Primary roads and trunk routes"),
    (helpers::Color, motorway, "#d9a441",
        "Motorway", "Motorways, drawn over a darker casing"),
    (helpers::Color, motorway_casing, "#8a5a1e",
        "Motorway Casing", "The outline under a motorway, which is what makes it read as one road rather than a stripe"),
    (helpers::Color, rail, "#3c3f47",
        "Rail", "Railway lines"),
    (helpers::Color, boundary, "#4a4f5c",
        "Boundary", "State and country borders"),

    // NAMED `racetrack_`, NOT `track_`. MapConfig_t already uses track_* for
    // the vehicle's own breadcrumb trail -- show_track, track_points,
    // track_width, track_opacity -- and a `track_width` on the style meaning
    // something else entirely would be unreadable in the properties panel.
    (helpers::Color, racetrack_surface, "#2a2f3a",
        "Racetrack Surface", "The tarmac of a race track. The infield is cut out as a hole, so this is the ribbon only"),
    (helpers::Color, racetrack_centre, "#7fd8ff",
        "Racetrack Centre Line", "The derived centre line, which is what a lap is measured along"),

    (helpers::Color, label_text, "#c3cad6",
        "Label Text", "Place name colour"),
    (helpers::Color, label_halo, "#12141a",
        "Label Halo", "Outline behind label text. Without it, names over a light area are unreadable"),
    (std::string, label_font, "Arial",
        "Label Font", "Family for place names. Naming one that exists spares Qt a font-alias scan at startup"),
    (uint16_t, label_size, 12,
        "Label Size", "Point size for place names"),
    (double, label_halo_width, 3.0,
        "Label Halo Width", "Stroke width of the halo in pixels. 0 draws the text bare"),
    (uint16_t, label_spacing, 4,
        "Label Spacing", "Clear space demanded around each label in pixels. Raise it to thin a crowded map"),

    (double, road_width_scale, 1.0,
        "Road Width", "Multiplier on every width in Widths. Raise it for a display seen at arm's length"),

    (bool, show_buildings, true,
        "Show Buildings", "Draw building footprints at high zoom"),
    (bool, show_labels, true,
        "Show Labels", "Draw place names"),
    (bool, show_boundaries, true,
        "Show Boundaries", "Draw state and country borders"),
    (bool, show_aeroways, true,
        "Show Aeroways", "Draw runways, taxiways and aprons. Without them an airport is a hole in the map"),
    (bool, show_bridges, true,
        "Show Bridges", "Draw bridges above the roads they cross. Off draws every road at grade, so an overpass and the street under it look the same"),
    (bool, show_road_labels, true,
        "Show Road Labels", "Draw street names. Each road is named once per viewport, horizontally, at the middle of its longest visible run"),
    (bool, show_racetracks, true,
        "Show Racetracks", "Draw race tracks. They come from their own archive, so this does nothing unless one is configured as an overlay tileset"),

    (MapWidths_t, widths, {},
        "Widths", "Per-layer line widths in pixels at zoom 14"),
    (MapDetail_t, detail, {},
        "Detail", "Per-layer lowest zoom. The archive's own thresholds still apply on top of these")
)

// Clamps rather than refuses, like every other config in this tree. A width
// scale of zero would draw a map with no roads on it, which reads as a broken
// tile pipeline rather than as a styling mistake.
inline std::vector<std::string> validate(MapStyle_t& style)
{
    std::vector<std::string> notes;
    config_codec::limits::clampInto<double>(style.road_width_scale, 0.1, 8.0,
                                            "road_width_scale", notes);
    config_codec::limits::clampInto<uint16_t>(style.label_size, 6u, 48u, "label_size", notes);
    config_codec::limits::clampInto<double>(style.label_halo_width, 0.0, 12.0,
                                            "label_halo_width", notes);
    config_codec::limits::clampInto<uint16_t>(style.label_spacing, 0u, 64u, "label_spacing", notes);

    const std::vector<std::string> widthNotes = validate(style.widths);
    notes.insert(notes.end(), widthNotes.begin(), widthNotes.end());
    const std::vector<std::string> detailNotes = validate(style.detail);
    notes.insert(notes.end(), detailNotes.begin(), detailNotes.end());

    return notes;
}

#endif // MAP_STYLE_H
