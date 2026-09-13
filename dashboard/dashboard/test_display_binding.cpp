// The display binding's decisions, against a fake environment.
//
// The property that matters most is the negative one: with nothing published --
// every desktop, every Mac, every --mcp run -- nothing here may decide to do
// anything, because the caller changes the process environment on a yes.

#include "dashboard/display_binding.h"

#include <cstdio>
#include <map>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using namespace dashboard::display;

EnvGetter fakeEnvironment(std::map<std::string, std::string> vars)
{
    return [captured = std::move(vars)](const std::string& name) -> std::optional<std::string>
    {
        if (const auto it = captured.find(name); it != captured.end())
        {
            return it->second;
        }
        return std::nullopt;
    };
}

// What the Rivian panel's boot actually publishes (read off the target).
const std::map<std::string, std::string> kRivianPrimary = {
    {"REDLINE_DISPLAY_PRIMARY_CONNECTOR", "HDMI-A-2"},
    {"REDLINE_DISPLAY_PRIMARY_PROFILE", "rivian-ic-la123wf9"},
    {"REDLINE_DISPLAY_PRIMARY_MODE", "1920x720"},
};

WindowPlacement window(display_role_t display, int width, int height, scale_mode_t scale = scale_mode_t::fit)
{
    WindowPlacement placement;
    placement.display = display;
    placement.scale = scale;
    placement.width = width;
    placement.height = height;
    return placement;
}

void testKmsScreenNames()
{
    // Measured on the LattePanda: eglfs_kms reports the screen on HDMI-A-2 as "HDMI2".
    check(kmsScreenName("HDMI-A-2") == "HDMI2", "HDMI-A-2 is HDMI2 under eglfs_kms");
    check(kmsScreenName("HDMI-A-1") == "HDMI1", "HDMI-A-1 is HDMI1");
    check(kmsScreenName("DP-1") == "DP1", "DP-1 is DP1");
    check(kmsScreenName("eDP-1") == "eDP1", "eDP-1 is eDP1");
    check(kmsScreenName("DVI-D-1") == "DVI1", "DVI-D-1 is DVI1");
    check(kmsScreenName("Virtual-1") == "Virtual1", "Virtual-1 is Virtual1");
    check(kmsScreenName("weird") == "weird", "a name with no index is left alone");
    check(screenMatchesConnector("HDMI2", "HDMI-A-2"), "the eglfs name matches the connector");
    check(screenMatchesConnector("HDMI-A-2", "HDMI-A-2"), "the Wayland name matches the connector");
    check(!screenMatchesConnector("HDMI1", "HDMI-A-2"), "another port does not match");
}

void testNothingPublishedMeansNothingToDo()
{
    const auto env = fakeEnvironment({{"QT_QPA_PLATFORM", "cocoa"}});
    check(!platformPublishesDisplays(env), "an environment with no display variables publishes no displays");
    check(!lookupDisplay(display_role_t::primary, env), "no primary display is found");
    check(!screenScaleFactors({window(display_role_t::primary, 1200, 450)}, env),
          "no scale factors are produced, so the environment is left alone");
}

void testTheRivianPanel()
{
    const auto env = fakeEnvironment(kRivianPrimary);
    check(platformPublishesDisplays(env), "a published primary is a published display");

    const auto primary = lookupDisplay(display_role_t::primary, env);
    check(primary.has_value() && primary->connector == "HDMI-A-2", "the primary connector is read");
    check(primary && primary->mode && primary->mode->width == 1920 && primary->mode->height == 720,
          "the primary mode is read");
    check(!lookupDisplay(display_role_t::secondary, env), "an unpublished secondary is absent");

    // The 190e cluster's design size on this panel: exactly 1.6 on both axes.
    const auto factors = screenScaleFactors({window(display_role_t::primary, 1200, 450)}, env);
    check(factors == std::optional<std::string>("HDMI-A-2=1.6;HDMI2=1.6"),
          "the 190e layout fits the Rivian panel at 1.6, got '" + factors.value_or("<none>") + "'");
}

void testFitLetterboxesTheLongerAxis()
{
    // 1920x1080 for a 1200x450 layout: 1.6 across, 2.4 down. The smaller wins,
    // so the width fills and the height letterboxes, and nothing is cropped.
    check(fitFactor({1920, 1080}, 1200, 450) == 1.6, "fit takes the smaller ratio");
    check(fitFactor({3840, 2160}, 1200, 450) == 3.2, "the 4K bench monitor fits at 3.2, as its unit pins");
    check(fitFactor({1920, 720}, 0, 450) == 1.0, "a degenerate design size does not divide by zero");
}

void testEachFittedWindowGetsItsOwnScreen()
{
    auto vars = kRivianPrimary;
    vars["REDLINE_DISPLAY_SECONDARY_CONNECTOR"] = "HDMI-A-1";
    vars["REDLINE_DISPLAY_SECONDARY_MODE"] = "1920x1080";
    const auto env = fakeEnvironment(vars);

    const auto factors = screenScaleFactors(
        {window(display_role_t::primary, 1200, 450), window(display_role_t::secondary, 800, 480)}, env);
    check(factors == std::optional<std::string>("HDMI-A-2=1.6;HDMI2=1.6;HDMI-A-1=2.25;HDMI1=2.25"),
          "two windows produce two named factors, got '" + factors.value_or("<none>") + "'");

    const auto unscaled = screenScaleFactors(
        {window(display_role_t::primary, 1200, 450),
         window(display_role_t::secondary, 800, 480, scale_mode_t::none)}, env);
    check(unscaled == std::optional<std::string>("HDMI-A-2=1.6;HDMI2=1.6"),
          "a `scale: none` window is left out, got '" + unscaled.value_or("<none>") + "'");
}

void testAConnectorWithoutAModeBindsButDoesNotScale()
{
    const auto env = fakeEnvironment({{"REDLINE_DISPLAY_PRIMARY_CONNECTOR", "HDMI-A-2"}});
    check(platformPublishesDisplays(env), "a connector alone is still a published display");
    check(!screenScaleFactors({window(display_role_t::primary, 1200, 450)}, env),
          "with no mode there is nothing to fit against");
}

void testMalformedModesAreRefused()
{
    for (const char* bad : {"", "1920", "x720", "1920x", "1920x720@60", " 1920x720", "0x720", "-1920x720", "axb"})
    {
        check(!parseMode(bad).has_value(), std::string("'") + bad + "' is not a mode");
    }
    check(parseMode("3840x2160").has_value(), "a 4K mode parses");
}

void testRoleNamesMapToTheRootfsVariables()
{
    check(environmentPrefix(display_role_t::primary) == "REDLINE_DISPLAY_PRIMARY_", "primary's prefix");
    check(environmentPrefix(display_role_t::secondary) == "REDLINE_DISPLAY_SECONDARY_", "secondary's prefix");
}

}  // namespace

int main()
{
    testKmsScreenNames();
    testNothingPublishedMeansNothingToDo();
    testTheRivianPanel();
    testFitLetterboxesTheLongerAxis();
    testEachFittedWindowGetsItsOwnScreen();
    testAConnectorWithoutAModeBindsButDoesNotScale();
    testMalformedModesAreRefused();
    testRoleNamesMapToTheRootfsVariables();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
