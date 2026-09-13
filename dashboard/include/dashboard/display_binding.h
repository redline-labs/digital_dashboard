#ifndef DASHBOARD_DISPLAY_BINDING_H_
#define DASHBOARD_DISPLAY_BINDING_H_

#include "dashboard/window_placement.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the rootfs says about the displays, and what the dashboard does with it.
//
// On the target, redline-display-setup detects the panels before the dashboard
// starts and the unit imports what it found:
//
//   REDLINE_DISPLAY_PRIMARY_CONNECTOR=HDMI-A-2
//   REDLINE_DISPLAY_PRIMARY_MODE=1920x720
//
// Anywhere else -- a desktop, a Mac, --mcp offscreen -- none of it is set, and
// every function here answers "nothing to do", which is what keeps those runs
// exactly as they were.
//
// Deliberately only a lookup: no DRM, no sysfs, no reading the kms.json. The
// rootfs owns detection; this owns placement. Pure C++ with the environment
// injected, so the decisions are unit-testable without Qt or a display.
namespace dashboard::display
{

// Reads one environment variable; nullopt when it is unset.
using EnvGetter = std::function<std::optional<std::string>(const std::string& name)>;

// The real process environment.
EnvGetter processEnvironment();

struct Mode
{
    int width = 0;
    int height = 0;
};

struct DisplayInfo
{
    // The DRM connector name, which is what QScreen::name() reports under
    // eglfs_kms and Wayland.
    std::string connector;

    // The panel's native mode, if the rootfs published one.
    std::optional<Mode> mode;
};

// "REDLINE_DISPLAY_PRIMARY_" for display_role_t::primary.
std::string environmentPrefix(display_role_t role);

// "1920x720" -> {1920, 720}. Anything else, including a zero dimension, is nullopt.
std::optional<Mode> parseMode(std::string_view text);

// The display the rootfs published for `role`, or nullopt if it published none.
std::optional<DisplayInfo> lookupDisplay(display_role_t role, const EnvGetter& env);

// True when the rootfs published any display at all -- i.e. this is a target
// with a detected display module, and windows should be bound rather than shown
// the way a desktop shows them.
bool platformPublishesDisplays(const EnvGetter& env);

// The uniform scale that fits a design size inside a mode: the smaller of the two
// ratios, so nothing is cropped and the longer axis letterboxes.
double fitFactor(Mode mode, int design_width, int design_height);

// A QT_SCREEN_SCALE_FACTORS value, e.g. "HDMI-A-2=1.6;HDMI-A-1=2.4", covering every
// `scale: fit` window whose display has both a connector and a mode. nullopt when
// there is nothing to scale, in which case the environment must be left alone.
//
// Per screen rather than one QT_SCALE_FACTOR, because two windows with different
// design sizes on two panels need two different factors, and QT_SCALE_FACTOR is
// one number for the whole process.
std::optional<std::string> screenScaleFactors(const std::vector<WindowPlacement>& windows,
                                              const EnvGetter& env);

}  // namespace dashboard::display

#endif  // DASHBOARD_DISPLAY_BINDING_H_
