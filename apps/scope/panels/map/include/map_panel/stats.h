#ifndef SCOPE_MAP_PANEL_STATS_H_
#define SCOPE_MAP_PANEL_STATS_H_

#include "reflection/reflection.h"

#include <cstdint>
#include <string>

// What the map actually drew, as opposed to what it was told to draw.
//
// READ-ONLY, like every stats struct, and served through `scope.stats` /
// `scope.describe_stats`, neither of which knows this type exists.
//
// A MAP IS THE PANEL A SCREENSHOT PROVES LEAST ABOUT. Every one of these is a
// separate reason the panel could be empty, and on screen they are the same
// picture:
//
//   nothing bound                     -> latitude_samples 0
//   two signals on different topics   -> unpaired_* climbing, paired_points 0
//   tileset not in Settings           -> diagnostic says so, tiles_requested 0
//   archive will not open             -> diagnostic carries the mbtiles error
//   camera over ground with no data   -> tiles_absent climbing, tiles_drawn 0
//   no GPU backend                    -> gpu_ready false
//
// And "the marker is in the wrong place" is not visible at all without
// marker_t: a marker drawn for an instant other than the shared cursor's looks
// exactly like a correct one.
REFLECT_STRUCT(MapPanelStats_t,
    (bool, latitude_bound, false,
        "Latitude Bound", "Whether the source accepted the latitude signal. False means "
                          "the expression did not compile or the subscription failed"),
    (bool, longitude_bound, false,
        "Longitude Bound", "As above, for longitude"),
    (bool, color_bound, false,
        "Colour Bound", "As above, for the optional colour signal"),

    (uint64_t, latitude_samples, 0,
        "Latitude Samples", "Latitudes currently retained"),
    (uint64_t, longitude_samples, 0,
        "Longitude Samples", "Longitudes currently retained"),
    (uint64_t, color_samples, 0,
        "Colour Samples", "Colour-signal samples currently retained"),
    (uint64_t, dropped, 0,
        "Dropped", "Samples the staging rings lost because the GUI did not drain them in "
                   "time, summed over the bound signals"),

    // THE FIELD THAT TURNS "the map is empty" INTO AN ANSWER. Thousands of
    // latitudes with zero paired points means the two signals are not on the
    // same topic and so share no message timestamp to pair on.
    (uint64_t, paired_points, 0,
        "Paired Points", "Positions built by matching a latitude and a longitude at the "
                         "same instant"),
    (uint64_t, unpaired_latitude, 0,
        "Unpaired Latitude", "Latitudes that found no longitude at their timestamp"),
    (uint64_t, unpaired_longitude, 0,
        "Unpaired Longitude", "Longitudes that found no latitude at their timestamp"),
    (uint64_t, track_points_drawn, 0,
        "Track Points Drawn", "Points left after pixel thinning, i.e. what the path holds"),

    (double, t_first, 0.0,
        "First Time (s)", "Source-clock time of the earliest position on the track"),
    (double, t_last, 0.0,
        "Last Time (s)", "Source-clock time of the latest position on the track"),

    (bool, marker_valid, false,
        "Marker Valid", "Whether a position exists at or before the readout instant"),
    (double, marker_t, 0.0,
        "Marker Time (s)", "The instant the marker is drawn for: the shared cursor when "
                           "there is one, otherwise the view's right edge. A marker drawn "
                           "for any other instant looks exactly like a correct one"),
    (double, marker_latitude, 0.0,
        "Marker Latitude", "Degrees north of the marker"),
    (double, marker_longitude, 0.0,
        "Marker Longitude", "Degrees east of the marker"),
    (bool, at_cursor, false,
        "At Cursor", "Whether the readout instant came from the shared cursor rather than "
                     "from the view's right edge"),

    // Where the map is ACTUALLY looking, which is not the configured centre once
    // Follow Cursor or a drag has moved it. A screenshot of the wrong place and
    // a screenshot of an archive with no coverage are the same picture.
    (double, camera_latitude, 0.0,
        "Camera Latitude", "Degrees north the camera is centred on"),
    (double, camera_longitude, 0.0,
        "Camera Longitude", "Degrees east the camera is centred on"),
    (double, camera_zoom, 0.0,
        "Camera Zoom", "The zoom the last frame was drawn at"),
    (double, camera_bearing, 0.0,
        "Camera Bearing", "Degrees clockwise from north the last frame was turned. Course "
                          "over ground in course-up mode"),
    (double, camera_pitch, 0.0,
        "Camera Pitch", "Degrees off straight-down the last frame was tilted. 0 in top-down"),
    (bool, camera_moved, false,
        "Camera Moved", "Whether a manual pan or zoom is in effect, overriding Follow Cursor"),

    (uint64_t, tiles_requested, 0,
        "Tiles Requested", "Tiles asked of the archive since the panel opened"),
    (uint64_t, tiles_decoded, 0,
        "Tiles Decoded", "Tiles that arrived and tessellated"),
    (uint64_t, tiles_absent, 0,
        "Tiles Absent", "Tiles the archive has nothing at. Expected and common: most of "
                        "the pyramid is empty"),
    (uint64_t, tiles_failed, 0,
        "Tiles Failed", "Tiles that could not be read or decoded. Unlike absent, a fault"),
    (uint64_t, tiles_drawn, 0,
        "Tiles Drawn", "Tiles the last frame actually drew"),
    (uint64_t, tiles_stand_in, 0,
        "Tiles Stand In", "Tiles drawn from another zoom to cover a gap. A number that "
                          "stays non-zero means tiles are not arriving"),
    (uint64_t, tiles_cached_bytes, 0,
        "Tiles Cached (bytes)", "What the decoded-tile cache is holding"),

    (bool, gpu_ready, false,
        "GPU Ready", "False means QRhi found no backend: the marker and the trail still "
                     "draw, the basemap does not"),

    // The caption the panel would print on itself. Empty when there is nothing
    // wrong, which is the assertion worth making.
    (std::string, diagnostic, "",
        "Diagnostic", "Why the map is empty, in the same words the panel paints on itself. "
                      "Empty means nothing is being captioned")
)

#endif  // SCOPE_MAP_PANEL_STATS_H_
