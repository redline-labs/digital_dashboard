#include "dashboard/app_config.h"

#include <spdlog/spdlog.h>

#include <string>

namespace {

using config_codec::Issue;

// Validates one entry of the `widgets:` list. The per-widget `config:` block is
// a different struct for every `type:`, so the walk has to dispatch, and
// The widget table is what makes that automatic for a widget added later.
void validateWidget(const YAML::Node& node, std::size_t index, std::vector<Issue>& issues)
{
    const std::string path = "widgets[" + std::to_string(index) + "]";

    if (!node.IsMap())
    {
        issues.push_back({Issue::Severity::error, path, "expected a mapping"});
        return;
    }

    // `type` is the one genuinely required key: without it there is no way to
    // know which config struct the `config:` block should be read as.
    if (!node["type"])
    {
        issues.push_back({Issue::Severity::error, path + ".type",
                          "missing; every widget entry needs a type"});
        return;
    }

    const std::string type_name = node["type"].as<std::string>();
    const auto type = reflection::enum_traits<widget_type_t>::try_from_string(type_name);
    if (!type || *type == widget_type_t::unknown)
    {
        // Built from the widget table rather than the enum, so `unknown` -- which
        // is an internal state, not something anyone should write in a file --
        // is not offered as a suggestion.
        std::string known;
#define KNOWN_TYPE(enum_name, widget_class)                                                             \
    if (!known.empty()) known += ", ";                                                       \
    known += std::string(reflection::enum_to_string(widget_class::kWidgetType));

        DASHBOARD_WIDGET_TABLE(KNOWN_TYPE)
#undef KNOWN_TYPE

        issues.push_back({Issue::Severity::error, path + ".type",
                          "unknown widget type '" + type_name + "'; expected one of: " + known});
        return;
    }

    // The placement keys, which live beside `type` rather than inside `config`.
    static constexpr std::string_view kPlacementKeys[] = {"type", "id", "x", "y", "width", "height", "config"};
    for (const auto& entry : node)
    {
        const std::string key = entry.first.as<std::string>();
        if (std::find(std::begin(kPlacementKeys), std::end(kPlacementKeys), key) == std::end(kPlacementKeys))
        {
            issues.push_back({Issue::Severity::warning, path + "." + key,
                              "unknown key, ignored"});
        }
    }

    if (!node["config"])
    {
        // Legal -- the widget takes its defaults -- but worth saying, because a
        // widget with no config is almost never what was meant.
        issues.push_back({Issue::Severity::warning, path + ".config",
                          "missing; the widget will use every default"});
        return;
    }

    const std::string cfg_path = path + ".config";
#define VALIDATE_CONFIG_CASE(enum_name, widget_class)                                                   \
    if (*type == widget_class::kWidgetType)                                                  \
    {                                                                                        \
        config_codec::detail::validateStruct<widget_class::config_t>(                   \
            node["config"], cfg_path, issues);                                               \
        return;                                                                              \
    }

    DASHBOARD_WIDGET_TABLE(VALIDATE_CONFIG_CASE)
#undef VALIDATE_CONFIG_CASE
}

}  // namespace

std::vector<Issue> validate_app_config(const YAML::Node& root)
{
    std::vector<Issue> issues;

    if (!root.IsMap())
    {
        issues.push_back({Issue::Severity::error, "", "the top level of a config must be a mapping"});
        return issues;
    }

    // The window-level keys, validated against app_config_t itself. `widgets` is
    // handled separately below because its element type depends on `type`.
    static constexpr std::string_view kWindowKeys[] = {"name", "width", "height", "background_color", "widgets"};
    for (const auto& entry : root)
    {
        const std::string key = entry.first.as<std::string>();
        if (std::find(std::begin(kWindowKeys), std::end(kWindowKeys), key) == std::end(kWindowKeys))
        {
            issues.push_back({Issue::Severity::warning, key, "unknown key, ignored"});
        }
    }

    // The window's own scalars. These cannot go through validateStruct against
    // app_config_t, because its `widgets` field is a vector whose element type
    // depends on `type` -- so they are checked individually here.
    if (root["background_color"])
    {
        const std::string color = root["background_color"].as<std::string>();
        if (!helpers::Color::isValidFormat(color))
        {
            // This one goes straight into a Qt stylesheet, where an unparseable
            // value makes Qt drop the whole rule and the window keeps whatever
            // background it had. Silently.
            issues.push_back({Issue::Severity::error, "background_color",
                              "'" + color + "' is not a colour; expected #RGB, #RRGGBB or #RRGGBBAA"});
        }
    }

    for (const char* key : {"width", "height"})
    {
        if (!root[key]) continue;
        try
        {
            (void)root[key].as<uint16_t>();
        }
        catch (const std::exception& e)
        {
            issues.push_back({Issue::Severity::error, key, e.what()});
        }
    }

    if (!root["widgets"])
    {
        issues.push_back({Issue::Severity::warning, "widgets", "missing; the window will be empty"});
        return issues;
    }

    if (!root["widgets"].IsSequence())
    {
        issues.push_back({Issue::Severity::error, "widgets", "expected a list"});
        return issues;
    }

    for (std::size_t i = 0; i < root["widgets"].size(); ++i)
    {
        validateWidget(root["widgets"][i], i, issues);
    }

    return issues;
}


std::optional<app_config_t> load_app_config(const std::string& config_filepath)
{
    // Default config in case of error.
    std::optional<app_config_t> config = std::nullopt;

    try
    {
        const YAML::Node root = YAML::LoadFile(config_filepath);

        // Validate before decoding. The decoder is driven by each struct's
        // fields, so anything the structs do not claim is invisible to it: a
        // mistyped key was dropped in silence and turned up much later as a
        // blank gauge. Report every problem in the file at once, with its path,
        // rather than aborting on the first one yaml-cpp happens to throw at.
        bool fatal = false;
        for (const auto& issue : validate_app_config(root))
        {
            const std::string where = issue.path.empty() ? config_filepath
                                                         : config_filepath + ": " + issue.path;
            if (issue.severity == config_codec::Issue::Severity::error)
            {
                fatal = true;
                SPDLOG_ERROR("{}: {}", where, issue.message);
            }
            else
            {
                SPDLOG_WARN("{}: {}", where, issue.message);
            }
        }

        if (fatal)
        {
            SPDLOG_CRITICAL("Refusing to load '{}': see the errors above.", config_filepath);
            return std::nullopt;
        }

        config = root.as<app_config_t>();
    }
    catch (const YAML::BadFile& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::BadFile : {})", e.what());
    }
    catch (const YAML::ParserException& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::ParserException : {})", e.what());
    }
    catch (const YAML::BadConversion& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::BadConversion : {})", e.what());
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::Exception : {})", e.what());
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to load app config: (std::exception : {})", e.what());
    }

    return config;
}