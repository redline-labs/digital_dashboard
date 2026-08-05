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

// Equality for anything declared with REFLECT_STRUCT, derived from the fields
// the same way the YAML conversion below is.
//
// The editor needs this to answer "did anything change?" without serialising the
// whole document to YAML and comparing strings -- which is what it used to do on
// every drag release and every title-bar update. It is also what lets an undo
// work out which widgets actually differ, so it can put one back where it was
// instead of rebuilding all of them.
//
// Writing these by hand was rejected once, reasonably: an operator== per widget
// config struct is a list that rots. Generated from the reflection data it
// cannot, and std::variant picks its own up for free once every alternative has
// one -- which is what makes widget_config_t below a three-line comparison
// rather than a switch over the widget table.
namespace dashboard::config::detail
{
// Compares field-by-field through the member pointers reflection already holds,
// rather than by walking both objects with visit_fields and matching names --
// which would be quadratic and would need the field types proved equal at each
// step. The fold short-circuits, so an early difference costs one comparison.
template <typename T, std::size_t... I>
bool fieldsEqual(const T& lhs, const T& rhs, std::index_sequence<I...>)
{
    const auto fields = T::reflection_fields();
    return (... && (lhs.*(std::get<I>(fields).member_ptr) ==
                    rhs.*(std::get<I>(fields).member_ptr)));
}
}  // namespace dashboard::config::detail

template <typename T>
    requires reflection::is_reflected_struct_v<T>
bool operator==(const T& lhs, const T& rhs)
{
    constexpr std::size_t kFieldCount =
        std::tuple_size_v<decltype(T::reflection_fields())>;
    return dashboard::config::detail::fieldsEqual(lhs, rhs,
                                                  std::make_index_sequence<kFieldCount>{});
}

inline bool operator==(const widget_config_t& lhs, const widget_config_t& rhs)
{
    return lhs.type == rhs.type && lhs.id == rhs.id && lhs.x == rhs.x && lhs.y == rhs.y &&
           lhs.width == rhs.width && lhs.height == rhs.height && lhs.config == rhs.config;
}


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
std::vector<dashboard::config::Issue> validate_app_config(const YAML::Node& root);

std::optional<app_config_t> load_app_config(const std::string& config_filepath);



#endif  // APP_CONFIG_H_
