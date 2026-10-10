// SPDX-License-Identifier: GPL-3.0-or-later
//
// map_render::Viewport: the camera policy both map surfaces share. Each rule
// here was once implemented twice and the copies disagreed, so each is pinned
// on its own.

#include "map_render/viewport.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::abs(a - b) <= tolerance;
}

using map_render::Coordinate;
using map_render::ScreenPoint;
using map_render::Viewport;

constexpr double kWidth = 800.0;
constexpr double kHeight = 600.0;
const Coordinate kIrvine { 33.6846, -117.8265 };
const Coordinate kVehicle { 33.70, -117.80 };

Viewport following()
{
    Viewport view;
    Viewport::Settings settings;
    settings.center = kIrvine;
    settings.zoom = 14.0;
    settings.min_zoom = 3.0;
    settings.max_zoom = 18.0;
    settings.follow = true;
    view.setSettings(settings);
    return view;
}

void theCentreIsTheUsersThenTheTargetThenTheConfigured()
{
    Viewport view = following();
    check(view.camera().center == kIrvine, "follow with no target yet: the configured centre");
    view.setTarget(kVehicle);
    check(view.camera().center == kVehicle, "follow with a target: the target");
    check(view.following(), "following before anyone touches it");

    view.setUserCentre(Coordinate { 34.0, -118.0 });
    check(view.camera().center == (Coordinate { 34.0, -118.0 }), "a user centre beats the target");
    check(view.moved() && !view.following(), "and suspends follow");

    view.recentre();
    check(view.camera().center == kVehicle && view.following(), "recentre goes back to following");

    Viewport::Settings settings = view.settings();
    settings.follow = false;
    view.setSettings(settings);
    check(view.camera().center == kIrvine, "without follow, the target is ignored");
    check(!view.following(), "and nothing is being followed");
}

void aDragKeepsTheGrabbedPointUnderThePointer()
{
    Viewport view = following();
    const ScreenPoint press { 200.0, 150.0 };
    const map_render::WorldPoint grabbed =
        map_render::Projection(view.camera(), kWidth, kHeight).worldForScreen(press);

    const ScreenPoint release { 520.0, 410.0 };
    view.moveSoThat(grabbed, release, kWidth, kHeight);
    const map_render::ScreenPoint landed =
        map_render::Projection(view.camera(), kWidth, kHeight).screenFor(grabbed);
    check(near(landed.x, release.x, 1e-6) && near(landed.y, release.y, 1e-6),
          "the grabbed point is under the pointer after the drag");
    check(view.moved(), "a drag suspends follow");
}

void aDragIsClampedAndWrapped()
{
    Viewport view = following();
    view.setUserCentre(Coordinate { 89.9, 190.0 });
    check(view.userCentre()->latitude < 85.06, "a centre past the pole is clamped to the Mercator limit");
    check(near(view.userCentre()->longitude, -170.0, 1e-9), "a centre past the date line wraps round it");

    // Through moveSoThat too, which is what scope's own copy skipped.
    Viewport dragged = following();
    Viewport::Settings settings = dragged.settings();
    settings.zoom = 3.0;
    dragged.setSettings(settings);
    const map_render::WorldPoint top { 0.5, -2.0 };
    dragged.moveSoThat(top, ScreenPoint { 400.0, 300.0 }, kWidth, kHeight);
    check(dragged.userCentre()->latitude <= 85.06 && dragged.userCentre()->latitude >= -85.06,
          "a drag off the top of the world stops at the pole");
}

void zoomingWhileFollowingKeepsFollowing()
{
    Viewport view = following();
    view.setTarget(kVehicle);
    check(view.zoomAbout(16.0, ScreenPoint { 10.0, 10.0 }, kWidth, kHeight), "the zoom changes");
    check(view.camera().zoom == 16.0, "to what was asked for");
    check(view.following() && view.camera().center == kVehicle,
          "about the centre: the target stays put and follow carries on");
    check(!view.moved(), "so there is nothing for recentre to undo");
}

void zoomingWhenNotFollowingKeepsThePointUnderThePointer()
{
    Viewport view = following();
    view.setUserCentre(kIrvine);
    const ScreenPoint at { 650.0, 120.0 };
    const map_render::WorldPoint before =
        map_render::Projection(view.camera(), kWidth, kHeight).worldForScreen(at);
    check(view.zoomAbout(15.5, at, kWidth, kHeight), "the zoom changes");
    const map_render::WorldPoint after =
        map_render::Projection(view.camera(), kWidth, kHeight).worldForScreen(at);
    check(near(before.x, after.x, 1e-12) && near(before.y, after.y, 1e-12),
          "the world point under the pointer is still under it");

    check(!view.zoomAbout(99.0, at, kWidth, kHeight) || view.camera().zoom == 18.0,
          "a zoom past the range stops at max_zoom");
    check(view.camera().zoom == 18.0, "at max_zoom");
    check(!view.zoomAbout(99.0, at, kWidth, kHeight), "and asking again changes nothing");
    view.setUserZoom(-4.0);
    check(view.camera().zoom == 3.0, "a user zoom is clamped to min_zoom");
}

void recentreKeepsTheZoom()
{
    Viewport view = following();
    view.setUserCentre(kIrvine);
    view.setUserZoom(17.0);
    view.recentre();
    check(view.camera().zoom == 17.0, "recentre returns the centre and leaves the zoom");
}

void theModes()
{
    Viewport view = following();
    view.setTrackBearing(90.0);
    check(view.camera().bearing == 0.0, "north-up ignores the track bearing");
    view.cycleOrientation();
    check(view.trackUp() && view.camera().bearing == 90.0, "track-up turns to it");

    view.setManualBearing(33.0);
    check(!view.trackUp() && view.camera().bearing == 33.0, "a manual spin forces north-up and sets the bearing");
    view.cycleOrientation();
    check(!view.trackUp() && view.camera().bearing == 0.0 && !view.manualBearing(),
          "the first click straightens, without changing mode");
    view.cycleOrientation();
    check(view.trackUp(), "the next click changes mode");

    view.setUserCentre(Coordinate { 34.0, -118.0 });
    check(view.trackUp(), "panning does not straighten the map");
    view.recentre();
    check(view.trackUp(), "and recentring does not either");

    Viewport::Settings settings = view.settings();
    settings.pitch = 45.0;
    view.setSettings(settings);
    check(view.camera().pitch == 0.0, "top-down ignores the pitch");
    view.toggleViewMode();
    check(view.camera().pitch == 45.0, "perspective uses it");
    view.toggleViewMode();
    check(view.camera().pitch == 0.0, "and back");
}

}  // namespace

int main()
{
    theCentreIsTheUsersThenTheTargetThenTheConfigured();
    aDragKeepsTheGrabbedPointUnderThePointer();
    aDragIsClampedAndWrapped();
    zoomingWhileFollowingKeepsFollowing();
    zoomingWhenNotFollowingKeepsThePointUnderThePointer();
    recentreKeepsTheZoom();
    theModes();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
