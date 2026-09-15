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
    // The DRM connector name ("HDMI-A-2"), which is what QScreen::name() reports
    // under Wayland. Qt's eglfs_kms backend names screens differently -- see
    // kmsScreenName() -- so a screen matches when its name equals either form.
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

// The name Qt's eglfs_kms backend gives the screen on a DRM connector: the
// connector type name followed by the connector's type index, with the
// subtype letter dropped -- "HDMI-A-2" -> "HDMI2", "DP-1" -> "DP1", "eDP-1" ->
// "eDP1", "DVI-D-1" -> "DVI1" (qkmsdevice.cpp's connector_type_names). Under
// Wayland QScreen::name() is the DRM name itself, so callers accept both.
std::string kmsScreenName(std::string_view connector);

// True when a QScreen named `screen_name` is the display on `connector`, in
// either naming scheme.
bool screenMatchesConnector(std::string_view screen_name, std::string_view connector);

// Whether any DRM connector under `drm_root` (/sys/class/drm) reports
// "connected". nullopt when there is no DRM at all (a desktop without sysfs,
// a Mac), in which case the caller must not draw conclusions. Used on the
// target to run headless when no display is attached: a bench unit with
// nothing plugged in is a valid configuration, not a broken one, and the
// dashboard must still report READY so the slot is not marked bad.
std::optional<bool> anyOutputConnected(std::string_view drm_root = "/sys/class/drm");

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
