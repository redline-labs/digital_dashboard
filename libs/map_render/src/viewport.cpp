// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_render/viewport.h"

#include <algorithm>

namespace map_render
{

Coordinate Viewport::homeCentre() const
{
    // Follow is a declared intent, not a state that waits for a fix: with no
    // target yet the configured centre stands in, and the first one moves it.
    if (settings_.follow && target_.has_value())
    {
        return *target_;
    }
    return settings_.center;
}

Camera Viewport::camera() const
{
    Camera out;
    out.center = user_centre_.value_or(homeCentre());
    out.zoom = user_zoom_.value_or(settings_.zoom);
    out.bearing = (trackUp() && track_bearing_.has_value()) ? *track_bearing_
                                                            : bearing_override_.value_or(settings_.bearing);
    out.pitch = viewMode() == MapViewMode_t::perspective ? settings_.pitch : 0.0;
    return out;
}

void Viewport::cycleOrientation()
{
    if (bearing_override_.has_value())
    {
        bearing_override_.reset();
        return;
    }
    // Stored even when it lands back on the configured mode: value_or makes the
    // two indistinguishable, and clearing it would only save an optional.
    track_up_override_ = !trackUp();
}

void Viewport::setManualBearing(double degrees)
{
    track_up_override_ = false;
    bearing_override_ = degrees;
}

void Viewport::toggleViewMode()
{
    switch (viewMode())
    {
    case MapViewMode_t::top_down:
        view_override_ = MapViewMode_t::perspective;
        break;
    case MapViewMode_t::perspective:
        view_override_ = MapViewMode_t::top_down;
        break;
    }
}

void Viewport::setUserCentre(const Coordinate& where)
{
    user_centre_ = Coordinate { clampLatitude(where.latitude), wrapLongitude(where.longitude) };
}

double Viewport::clampZoom(double zoom) const
{
    // The CAMERA's range, the layout's business. Closer than the archive goes
    // is fine: the deepest tiles are drawn magnified, and vector tiles stay
    // sharp.
    return std::clamp(zoom, settings_.min_zoom, std::max(settings_.min_zoom, settings_.max_zoom));
}

void Viewport::moveSoThat(const WorldPoint& world, const ScreenPoint& screen, double width, double height,
                          double device_pixel_ratio)
{
    const Projection projection(camera(), width, height, device_pixel_ratio);
    // What is under the point NOW; the difference to where it should be is
    // exactly how far the centre moves.
    const WorldPoint under = projection.worldForScreen(screen);
    const WorldPoint centre = worldFor(projection.camera().center);
    setUserCentre(coordinateFor(WorldPoint { centre.x + (world.x - under.x), centre.y + (world.y - under.y) }));
}

bool Viewport::zoomAbout(double zoom, const ScreenPoint& screen, double width, double height,
                         double device_pixel_ratio)
{
    const Camera before = camera();
    const double wanted = clampZoom(zoom);
    if (wanted == before.zoom)
    {
        return false;
    }
    if (following())
    {
        user_zoom_ = wanted;
        return true;
    }
    const WorldPoint anchor = Projection(before, width, height, device_pixel_ratio).worldForScreen(screen);
    user_zoom_ = wanted;
    moveSoThat(anchor, screen, width, height, device_pixel_ratio);
    return true;
}

} // namespace map_render
