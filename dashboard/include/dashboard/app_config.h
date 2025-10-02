#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "reflection/reflection.h"

#include "editor/widget_registry_list.h"
#include "carplay/carplay_widget.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "mercedes_190e_telltales/telltale.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include "motec_c125_tachometer/motec_c125_tachometer.h"
#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"
#include "static_text/static_text.h"
#include "value_readout/value_readout.h"
#include "background_rect/background_rect.h"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

REFLECT_ENUM(widget_type_t,
    mercedes_190e_speedometer,
    mercedes_190e_tachometer,
    mercedes_190e_telltale,
    mercedes_190e_cluster_gauge,
    motec_c125_tachometer,
    motec_cdl3_tachometer,
    static_text,
    value_readout,
    background_rect,
    sparkline,
    carplay,
    unknown
)

struct widget_config_t {
    widget_config_t() :
        type{widget_type_t::unknown},
        x{0},
        y{0},
        width{100},
        height{100},
        config{}
    {}

    widget_type_t type;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;

    std::variant<
        Mercedes190ESpeedometer::config_t,
        CarPlayWidget::config_t,
        Mercedes190ETachometer::config_t,
        Mercedes190ETelltale::config_t,
        SparklineItem::config_t,
        Mercedes190EClusterGauge::config_t,
        MotecC125Tachometer::config_t,
        MotecCdl3Tachometer::config_t,
        StaticTextWidget::config_t,
        ValueReadoutWidget::config_t,
        BackgroundRectWidget::config_t> config;
};

REFLECT_STRUCT(app_config_t,
    (std::string, name, ""),
    (uint16_t, width, 800),
    (uint16_t, height, 480),
    (std::string, background_color, "#000000"),
    (std::vector<widget_config_t>, widgets, {})
)


#define YAML_CONFIG_STRUCT(reflect_struct_name) \
template<> \
struct convert<reflect_struct_name> \
{ \
    static Node encode(const reflect_struct_name& rhs) \
    { \
        Node node = {}; \
        reflection::visit_fields<reflect_struct_name>(rhs, [&](std::string_view name, const auto& ref, std::string_view /*type*/) \
        { \
            node[name] = ref; \
        }); \
        return node; \
    } \
    static bool decode(const Node& node, reflect_struct_name& rhs) \
    { \
        if (!node.IsMap()) return false; \
        reflection::visit_fields<reflect_struct_name>(rhs, [&](std::string_view name, auto& ref, std::string_view /*type*/) \
        { \
            if (node[name]) \
            { \
                ref = node[name].as<std::decay_t<decltype(ref)>>(); \
            } \
        }); \
        return true; \
    } \
}

#define YAML_CONFIG_ENUM(reflect_enum_name) \
template<> \
struct convert<reflect_enum_name> \
{ \
    static Node encode(const reflect_enum_name& rhs) \
    { \
        return YAML::Node(reflection::enum_to_string(rhs)); \
    } \
    static bool decode(const Node& node, reflect_enum_name& rhs) \
    { \
        try \
        { \
            rhs = reflection::enum_traits<reflect_enum_name>::from_string(node.as<std::string>()); \
            return true; \
        } \
        catch (const std::exception& e) \
        { \
            SPDLOG_ERROR("{}.", e.what()); \
            return false; \
        } \
    } \
}


// Convert from a YAML Node to a native config_t.
namespace YAML {

YAML_CONFIG_ENUM(pub_sub::schema_type_t);

YAML_CONFIG_ENUM(ValueReadoutAlignment);
YAML_CONFIG_STRUCT(ValueReadoutConfig_t);

YAML_CONFIG_ENUM(GradientDirection);
YAML_CONFIG_STRUCT(BackgroundRectConfig_t);

YAML_CONFIG_STRUCT(StaticTextConfig_t);

YAML_CONFIG_STRUCT(Mercedes190ESpeedometerConfig_t);
YAML_CONFIG_STRUCT(Mercedes190ETachometerConfig_t);
YAML_CONFIG_STRUCT(MotecC125TachometerConfig_t);
YAML_CONFIG_STRUCT(MotecCdl3TachometerConfig_t);
YAML_CONFIG_STRUCT(SparklineConfig_t);

YAML_CONFIG_ENUM(Mercedes190ETelltaleType);
YAML_CONFIG_STRUCT(Mercedes190ETelltaleConfig_t);

YAML_CONFIG_STRUCT(sub_gauge_config_t);
YAML_CONFIG_STRUCT(Mercedes190EClusterGaugeConfig_t);

YAML_CONFIG_ENUM(DriveType);
YAML_CONFIG_ENUM(WiFiType);
YAML_CONFIG_ENUM(MicType);
YAML_CONFIG_STRUCT(phone_config_t);
YAML_CONFIG_STRUCT(carplay_phone_config_t);
YAML_CONFIG_STRUCT(android_auto_phone_config_t);
YAML_CONFIG_STRUCT(CarplayConfig_t);

YAML_CONFIG_STRUCT(app_config_t);



template<>
struct convert<widget_config_t> {
    static Node encode(const widget_config_t& rhs)
    {
        Node node = {};

        node["x"] = rhs.x;
        node["y"] = rhs.y;
        node["width"] = rhs.width;
        node["height"] = rhs.height;

        node["type"] = reflection::enum_to_string(rhs.type);

        // Use std::visit to encode whichever config is active in the variant
        std::visit([&](const auto& cfg) {
            node["config"] = cfg;
        }, rhs.config);

        return node;
    }

    static bool decode(const Node& node, widget_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        std::string type = node["type"].as<std::string>();
        
        if (node["x"]) rhs.x = node["x"].as<int16_t>();
        if (node["y"]) rhs.y = node["y"].as<int16_t>();
        if (node["width"]) rhs.width = node["width"].as<uint16_t>();
        if (node["height"]) rhs.height = node["height"].as<uint16_t>();

        // Use FOR_EACH_WIDGET to generate if-else chain
        bool matched = false;
        
#define DECODE_CONFIG_IF(enum_val, widget_class, label) \
        if (!matched && type == widget_class::kWidgetName) { \
            rhs.type = widget_type_t::enum_val; \
            rhs.config = node["config"].as<widget_class::config_t>(); \
            matched = true; \
        }
        
        FOR_EACH_WIDGET(DECODE_CONFIG_IF)
#undef DECODE_CONFIG_IF
        
        // Backward compatibility for old widget type names
        if (!matched && type == "mercedes_190e_battery_telltale") {
            rhs.type = widget_type_t::mercedes_190e_telltale;
            rhs.config = node["config"].as<Mercedes190ETelltale::config_t>();
            matched = true;
        }
        
        if (!matched) {
            SPDLOG_WARN("Unknown widget type '{}', unable to parse config.", type);
            rhs.type = widget_type_t::unknown;
        }

        return true;
    }
};

}   // namespace YAML


std::optional<app_config_t> load_app_config(const std::string& config_filepath);



#endif  // APP_CONFIG_H_
