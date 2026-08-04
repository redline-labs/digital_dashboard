#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "reflection/reflection.h"
#include "dashboard/config_validation.h"
#include "dashboard/widget_types.h"
#include "helpers/color.h"

#include "editor/widget_registry.h"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

struct widget_config_t {
    widget_config_t() :
        type{widget_type_t::unknown},
        x{0},
        y{0},
        width{100},
        height{100},
        config{std::monostate{}}
    {}

    widget_type_t type;

    // Optional stable handle for tooling (the agent control interface addresses
    // widgets as "#<id>"). Empty means "unnamed": the widget still gets an
    // objectName, derived as "<type>#<index>", but that one shifts when widgets
    // are added or reordered.
    std::string id;

    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;

    // Alternatives are derived from the widget registry; std::monostate is the
    // "unknown" state (and absorbs the trailing comma from the macro).
#define WIDGET_CONFIG_ALT(enum_name, widget_class) widget_class::config_t,
    std::variant<DASHBOARD_WIDGET_TABLE(WIDGET_CONFIG_ALT) std::monostate> config;
#undef WIDGET_CONFIG_ALT
};

REFLECT_STRUCT(app_config_t,
    (std::string, name, ""),
    (uint16_t, width, 800),
    (uint16_t, height, 480),
    (helpers::Color, background_color, "#000000"),
    (std::vector<widget_config_t>, widgets, {})
)


// Convert from a YAML Node to a native config_t.
namespace YAML {

// Every REFLECT_STRUCT and REFLECT_ENUM converts to and from YAML, without
// being named anywhere.
//
// These used to be per-type macro invocations -- one line for each widget
// config, each nested struct and each enum. That list was pure duplication of
// what the reflection macros already declare, and it failed open: leaving a
// type out is not a missing-registration error but an "implicit instantiation
// of undefined template" from deep inside yaml-cpp, pointing at the header that
// happened to instantiate it rather than at the type. Nested structs and enums
// were the usual casualty, because adding one to an existing config does not
// look like it should need registering anywhere.
//
// The argument list here is identical to yaml-cpp's primary template, which is
// normally ill-formed for a partial specialization. C++20 permits it when the
// specialization is more constrained ([temp.spec.partial]), which is what the
// requires-clauses provide. Hand-written full specializations -- widget_config_t
// below, helpers::Color -- are more specialized still and continue to win.
template <typename T>
    requires reflection::is_reflected_struct_v<T>
struct convert<T>
{
    static Node encode(const T& rhs)
    {
        Node node = {};
        reflection::visit_fields<T>(rhs, [&](std::string_view name, const auto& ref, std::string_view /*type*/)
        {
            node[name] = ref;
        });
        return node;
    }

    static bool decode(const Node& node, T& rhs)
    {
        if (!node.IsMap()) return false;
        reflection::visit_fields<T>(rhs, [&](std::string_view name, auto& ref, std::string_view /*type*/)
        {
            if (node[name])
            {
                ref = node[name].as<std::decay_t<decltype(ref)>>();
            }
        });
        return true;
    }
};

template <typename T>
    requires reflection::is_reflected_enum_v<T>
struct convert<T>
{
    static Node encode(const T& rhs)
    {
        return YAML::Node(reflection::enum_to_string(rhs));
    }

    static bool decode(const Node& node, T& rhs)
    {
        // Silent on failure by design: validate_app_config() reports the same
        // problem with the field's path and the valid alternatives, and two
        // messages for one typo is worse than one good one.
        try
        {
            const auto value = reflection::enum_traits<T>::try_from_string(node.as<std::string>());
            if (!value) return false;
            rhs = *value;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
};



template<>
struct convert<widget_config_t> {
    static Node encode(const widget_config_t& rhs)
    {
        Node node = {};

        // Omitted entirely when unset, so a config saved by the editor stays
        // byte-identical to one that never had an id.
        if (!rhs.id.empty())
        {
            node["id"] = rhs.id;
        }

        node["x"] = rhs.x;
        node["y"] = rhs.y;
        node["width"] = rhs.width;
        node["height"] = rhs.height;

        node["type"] = reflection::enum_to_string(rhs.type);

        // Use std::visit to encode whichever config is active in the variant
        std::visit([&](const auto& cfg) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(cfg)>, std::monostate>)
            {
                node["config"] = cfg;
            }
        }, rhs.config);

        return node;
    }

    static bool decode(const Node& node, widget_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        std::string type = node["type"].as<std::string>();
        
        if (node["id"]) rhs.id = node["id"].as<std::string>();
        if (node["x"]) rhs.x = node["x"].as<int16_t>();
        if (node["y"]) rhs.y = node["y"].as<int16_t>();
        if (node["width"]) rhs.width = node["width"].as<uint16_t>();
        if (node["height"]) rhs.height = node["height"].as<uint16_t>();

        // Use the widget table to generate the if-else chain
        bool matched = false;
        
#define DECODE_CONFIG_IF(enum_name, widget_class) \
        if (!matched && type == reflection::enum_to_string(widget_class::kWidgetType)) { \
            rhs.type = widget_class::kWidgetType; \
            rhs.config = node["config"].as<widget_class::config_t>(); \
            matched = true; \
        }
        
        DASHBOARD_WIDGET_TABLE(DECODE_CONFIG_IF)
#undef DECODE_CONFIG_IF

        if (!matched) {
            SPDLOG_WARN("Unknown widget type '{}', unable to parse config.", type);
            rhs.type = widget_type_t::unknown;
        }

        return true;
    }
};

}   // namespace YAML


// Checks a parsed config tree and returns everything wrong with it, each with a
// path like "widgets[3].config.zenoh_key". Errors mean the file cannot be loaded
// as written; warnings mean something in it was ignored.
std::vector<dashboard::config::Issue> validate_app_config(const YAML::Node& root);

std::optional<app_config_t> load_app_config(const std::string& config_filepath);



#endif  // APP_CONFIG_H_
