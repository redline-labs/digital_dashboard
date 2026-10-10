// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where a map surface is looking, and what the user has done to that.
#ifndef MAP_RENDER_VIEWPORT_H
#define MAP_RENDER_VIEWPORT_H

#include "map_render/camera_modes.h"
#include "map_render/projection.h"

#include <optional>

namespace map_render
{

// The camera policy the dashboard's map widget and scope's map panel share.
//
// They held a copy each, and the copies had drifted: scope's drag left a centre
// dragged past a pole or across the date line unnormalised, its zoom buttons
// claimed to suspend Follow Cursor and did not, and its "camera moved" stat
// disagreed with its own recentre button. One policy, with the dashboard's
// semantics because they were the ones with tests:
//
//   * The camera centre is the user's, else the followed target, else the
//     configured centre. A drag sets the user's centre, and having one IS what
//     "following is suspended" means -- there is no second flag.
//   * Zooming while following zooms about the centre and keeps following; the
//     target does not move on screen, so there is nothing to suspend.
//   * Recentre clears the user's centre and keeps their zoom.
//   * Orientation is independent of the centre: panning does not straighten
//     the map, recentring does not turn it.
//
// What "track up" follows is the host's business -- the vehicle's live heading
// on the dashboard, the course under the cursor in scope -- and it hands the
// bearing over with setTrackBearing(). No Qt, no clock: an ease is the host's,
// driving setUserZoom()/moveSoThat() a step at a time.
class Viewport
{
  public:
    // What a layout configures: where the map opens and the range it may move
    // in. Overrides never write back to it.
    struct Settings
    {
        Coordinate center {};
        double zoom { 13.0 };
        double bearing { 0.0 };
        double pitch { 0.0 };
        double min_zoom { 0.0 };
        double max_zoom { 22.0 };
        bool follow { false };
        bool track_up { false };
        MapViewMode_t view_mode { MapViewMode_t::top_down };
    };

    void setSettings(const Settings& settings) { settings_ = settings; }
    const Settings& settings() const { return settings_; }

    // What follow centres on: the vehicle, or the marker under the cursor.
    // nullopt until there is one, and follow keeps the configured centre.
    void setTarget(std::optional<Coordinate> target) { target_ = target; }
    const std::optional<Coordinate>& target() const { return target_; }
    // The bearing track-up turns to; nullopt leaves the map at its north-up
    // bearing until one arrives.
    void setTrackBearing(std::optional<double> degrees) { track_bearing_ = degrees; }

    Camera camera() const;
    // The centre follow would put the camera on now: the target when following
    // and there is one, the configured centre otherwise. Where recentre lands.
    Coordinate homeCentre() const;

    // ------------------------------------------------------------- modes
    bool trackUp() const { return track_up_override_.value_or(settings_.track_up); }
    MapViewMode_t viewMode() const { return view_override_.value_or(settings_.view_mode); }
    // A compass click straightens first: a manually spun map takes one click
    // to un-spin, and only the next click changes mode.
    void cycleOrientation();
    // Grabbing the needle is taking manual control, so it forces north-up.
    void setManualBearing(double degrees);
    std::optional<double> manualBearing() const { return bearing_override_; }
    void toggleViewMode();

    // ------------------------------------------------------ interaction
    // Clamped to the pole limit and wrapped round the date line: a drag past
    // either produces a coordinate Web Mercator has no answer for.
    void setUserCentre(const Coordinate& where);
    const std::optional<Coordinate>& userCentre() const { return user_centre_; }
    // Clamped to the configured range.
    void setUserZoom(double zoom) { user_zoom_ = clampZoom(zoom); }
    std::optional<double> userZoom() const { return user_zoom_; }
    double clampZoom(double zoom) const;

    // Moves the centre so that `world` lands under `screen` in a viewport of
    // this size, at the current zoom and bearing. In world units through the
    // projection's own inverse, so it is right at every zoom and under
    // rotation and pitch. What a drag calls with the point grabbed at press.
    void moveSoThat(const WorldPoint& world, const ScreenPoint& screen, double width, double height,
                    double device_pixel_ratio = 1.0);

    // Zooms to `zoom` (clamped). While following(), about the centre, so the
    // target stays put and following carries on; otherwise about `screen`,
    // which keeps the point under it where it is. False when the clamped zoom
    // is where the camera already is.
    bool zoomAbout(double zoom, const ScreenPoint& screen, double width, double height,
                   double device_pixel_ratio = 1.0);

    // Back to the target or the configured centre; the zoom stays where the
    // user put it.
    void recentre() { user_centre_.reset(); }
    // Forgets everything the user did to the camera -- centre and zoom -- but
    // not the mode buttons. For a host whose target means something else now.
    void forgetUserCamera()
    {
        user_centre_.reset();
        user_zoom_.reset();
    }
    // The user has put the centre somewhere: what the recentre button undoes,
    // and the only thing that suspends follow.
    bool moved() const { return user_centre_.has_value(); }
    // Follow is on and not suspended. A declared intent, not a state that
    // waits for a target: true before the first fix too.
    bool following() const { return settings_.follow && !moved(); }

  private:
    Settings settings_;
    std::optional<Coordinate> target_;
    std::optional<double> track_bearing_;
    std::optional<Coordinate> user_centre_;
    std::optional<double> user_zoom_;
    std::optional<bool> track_up_override_;
    std::optional<MapViewMode_t> view_override_;
    std::optional<double> bearing_override_;
};

} // namespace map_render

#endif // MAP_RENDER_VIEWPORT_H
