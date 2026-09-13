#include "dashboard/display_binding.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>

namespace dashboard::display
{

EnvGetter processEnvironment()
{
    return [](const std::string& name) -> std::optional<std::string>
    {
        if (const char* value = std::getenv(name.c_str()))
        {
            return std::string(value);
        }
        return std::nullopt;
    };
}

std::string environmentPrefix(display_role_t role)
{
    std::string upper(reflection::enum_to_string(role));
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return "REDLINE_DISPLAY_" + upper + "_";
}

std::optional<Mode> parseMode(std::string_view text)
{
    const std::size_t x = text.find('x');
    if (x == std::string_view::npos)
    {
        return std::nullopt;
    }

    // from_chars must consume each half exactly, so "1920x720@60" and " 1920x720"
    // are refused rather than half-read.
    const auto parseDimension = [](std::string_view part) -> std::optional<int>
    {
        int value = 0;
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (ec != std::errc{} || end != part.data() + part.size() || value <= 0)
        {
            return std::nullopt;
        }
        return value;
    };

    const auto width = parseDimension(text.substr(0, x));
    const auto height = parseDimension(text.substr(x + 1));
    if (!width || !height)
    {
        return std::nullopt;
    }
    return Mode{*width, *height};
}

std::optional<DisplayInfo> lookupDisplay(display_role_t role, const EnvGetter& env)
{
    const std::string prefix = environmentPrefix(role);

    const auto connector = env(prefix + "CONNECTOR");
    if (!connector || connector->empty())
    {
        return std::nullopt;
    }

    DisplayInfo info;
    info.connector = *connector;
    if (const auto mode = env(prefix + "MODE"))
    {
        info.mode = parseMode(*mode);
    }
    return info;
}

std::string kmsScreenName(std::string_view connector)
{
    // "<type>[-<subtype>]-<index>": keep the type, drop the subtype, append the
    // index without a separator. Anything that does not end in "-<digits>" is
    // returned unchanged; there is nothing better to guess.
    const auto dash = connector.rfind('-');
    if (dash == std::string_view::npos || dash + 1 >= connector.size())
    {
        return std::string(connector);
    }
    const std::string_view index = connector.substr(dash + 1);
    if (!std::all_of(index.begin(), index.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
    {
        return std::string(connector);
    }
    std::string_view type = connector.substr(0, dash);
    const auto type_dash = type.find('-');
    if (type_dash != std::string_view::npos)
    {
        type = type.substr(0, type_dash);  // "HDMI-A" -> "HDMI", "DVI-D" -> "DVI"
    }
    return std::string(type) + std::string(index);
}

bool screenMatchesConnector(std::string_view screen_name, std::string_view connector)
{
    return screen_name == connector || screen_name == kmsScreenName(connector);
}

bool platformPublishesDisplays(const EnvGetter& env)
{
    for (const display_role_t role : enum_values(display_role_t{}))
    {
        if (lookupDisplay(role, env))
        {
            return true;
        }
    }
    return false;
}

double fitFactor(Mode mode, int design_width, int design_height)
{
    if (mode.width <= 0 || mode.height <= 0 || design_width <= 0 || design_height <= 0)
    {
        return 1.0;
    }
    return std::min(static_cast<double>(mode.width) / design_width,
                    static_cast<double>(mode.height) / design_height);
}

std::optional<std::string> screenScaleFactors(const std::vector<WindowPlacement>& windows,
                                              const EnvGetter& env)
{
    std::string out;
    for (const WindowPlacement& window : windows)
    {
        if (window.scale != scale_mode_t::fit)
        {
            continue;
        }

        const auto info = lookupDisplay(window.display, env);
        if (!info || !info->mode)
        {
            continue;
        }

        char factor[32];
        std::snprintf(factor, sizeof(factor), "%.6g",
                      fitFactor(*info->mode, window.width, window.height));

        // Qt keys this variable by QScreen::name(), which is the DRM name under
        // Wayland and eglfs_kms's own spelling on the target; list both, Qt
        // ignores a name that matches no screen.
        if (!out.empty())
        {
            out += ';';
        }
        out += info->connector + "=" + factor;
        const std::string kms = kmsScreenName(info->connector);
        if (kms != info->connector)
        {
            out += ";" + kms + "=" + factor;
        }
    }

    if (out.empty())
    {
        return std::nullopt;
    }
    return out;
}

}  // namespace dashboard::display
