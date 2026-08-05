#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "reflection/reflection.h"
#include "config_codec/config_validation.h"
#include "config_codec/config_yaml.h"
#include "dashboard/widget_types.h"
#include "helpers/color.h"

#include "editor/widget_registry.h"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

// A widget's configuration, whichever kind it is. Alternatives are derived from
// the widget registry; std::monostate is the "unknown widget type" state (and
// absorbs the trailing comma from the macro).
//
// Named rather than declared inline inside widget_config_t because the editor
// stores one: a SelectionFrame holds the configuration it was given, so that
// exporting a document does not have to read it back out of the live widget.
#define WIDGET_CONFIG_ALT(enum_name, widget_class) widget_class::config_t,
using widget_config_variant_t =
    std::variant<DASHBOARD_WIDGET_TABLE(WIDGET_CONFIG_ALT) std::monostate>;
#undef WIDGET_CONFIG_ALT

// The variant holding a default-constructed config of the right kind for `type`,
// or monostate if the type is unknown.
//
// Sits here rather than beside instantiateWidget() in widget_registry.h only
// because widget_config_variant_t is declared here and app_config.h is the one
// that includes the registry, not the other way round.
inline widget_config_variant_t default_widget_config(widget_type_t type)
{
    widget_config_variant_t config{std::monostate{}};
    switch (type)
    {
#define WIDGET_DEFAULT_CONFIG_CASE(enum_name, widget_class) \
        case widget_class::kWidgetType: \
            config = typename widget_class::config_t{}; \
            break;

        DASHBOARD_WIDGET_TABLE(WIDGET_DEFAULT_CONFIG_CASE)
#undef WIDGET_DEFAULT_CONFIG_CASE

        case widget_type_t::unknown:
        default:
            break;
    }
    return config;
}

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

    widget_config_variant_t config;
};

REFLECT_STRUCT(app_config_t,
    (std::string, name, ""),
    (uint16_t, width, 800),
    (uint16_t, height, 480),
    (helpers::Color, background_color, "#000000"),
    (std::vector<widget_config_t>, widgets, {})
)

// The generic half of this -- operator== and the YAML conversions for every
// reflected struct and enum -- lives in config_codec/config_yaml.h. Only the
// widget-specific pieces are below.
inline bool operator==(const widget_config_t& lhs, const widget_config_t& rhs)
{
    return lhs.type == rhs.type && lhs.id == rhs.id && lhs.x == rhs.x && lhs.y == rhs.y &&
           lhs.width == rhs.width && lhs.height == rhs.height && lhs.config == rhs.config;
}


// Convert from a YAML Node to a native config_t.
namespace YAML {

// The generic reflected-struct and reflected-enum specializations are in
// config_codec/config_yaml.h. This full specialization is more specialized than
// either and continues to win over them.
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
        
        // A missing `config:` is legal and means "every default" -- validate_widget()
        // says so in as many words and only warns about it. The decoder disagreed:
        // it read node["config"] unconditionally, and yaml-cpp throws on an
        // undefined node, so the exception escaped to load_app_config and failed the
        // whole file. A config the validator passes has to be a config the decoder
        // accepts, or the validation is theatre.
        //
        // Default-CONSTRUCT rather than leave the variant empty. std::monostate has
        // exactly one meaning downstream -- "unknown widget type, construct nothing"
        // (see widget_factory.h) -- so parking a known type on it would render the
        // widget in the editor and silently omit it from the dashboard.
#define DECODE_CONFIG_IF(enum_name, widget_class) \
        if (!matched && type == reflection::enum_to_string(widget_class::kWidgetType)) { \
            rhs.type = widget_class::kWidgetType; \
            rhs.config = node["config"] ? node["config"].as<widget_class::config_t>() \
                                        : widget_class::config_t{}; \
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
std::vector<config_codec::Issue> validate_app_config(const YAML::Node& root);

std::optional<app_config_t> load_app_config(const std::string& config_filepath);



#endif  // APP_CONFIG_H_
