#include "dashboard/app_config.h"
#include <yaml-cpp/yaml.h>
#include "reflection/reflection.h"

#include <spdlog/spdlog.h>

#include <string>


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

YAML_CONFIG_STRUCT(window_config_t);
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

        if (rhs.type == widget_type_t::carplay)
        {
            node["config"] = std::get<CarPlayWidget::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::mercedes_190e_speedometer)
        {
            node["config"] = std::get<Mercedes190ESpeedometer::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::mercedes_190e_tachometer)
        {
            node["config"] = std::get<Mercedes190ETachometer::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::sparkline)
        {
            node["config"] = std::get<SparklineItem::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::mercedes_190e_telltale)
        {
            node["config"] = std::get<Mercedes190ETelltale::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::mercedes_190e_cluster_gauge)
        {
            node["config"] = std::get<Mercedes190EClusterGauge::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::motec_c125_tachometer)
        {
            node["config"] = std::get<MotecC125Tachometer::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::motec_cdl3_tachometer)
        {
            node["config"] = std::get<MotecCdl3Tachometer::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::static_text)
        {
            node["config"] = std::get<StaticTextWidget::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::value_readout)
        {
            node["config"] = std::get<ValueReadoutWidget::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::background_rect)
        {
            node["config"] = std::get<BackgroundRectWidget::config_t>(rhs.config);
        }
        else
        {
            SPDLOG_WARN("Unknown widget type '{}', unable to parse config.", reflection::enum_to_string(rhs.type));
            node["type"] = "unknown";
        }

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

        if (type == CarPlayWidget::kWidgetName)
        {
            rhs.type = widget_type_t::carplay;
            rhs.config = node["config"].as<CarplayConfig_t>();
        }
        else if (type == Mercedes190ESpeedometer::kWidgetName)
        {
            rhs.type = widget_type_t::mercedes_190e_speedometer;
            rhs.config = node["config"].as<Mercedes190ESpeedometerConfig_t>();
        }
        else if (type == Mercedes190ETachometer::kWidgetName)
        {
            rhs.type = widget_type_t::mercedes_190e_tachometer;
            rhs.config = node["config"].as<Mercedes190ETachometerConfig_t>();
        }
        else if (type == SparklineItem::kWidgetName)
        {
            rhs.type = widget_type_t::sparkline;
            rhs.config = node["config"].as<SparklineConfig_t>();
        }
        else if (type == Mercedes190ETelltale::kWidgetName)
        {
            rhs.type = widget_type_t::mercedes_190e_telltale;
            rhs.config = node["config"].as<Mercedes190ETelltaleConfig_t>();
        }
        else if (type == std::string("mercedes_190e_battery_telltale"))
        {
            // Backward compatibility with old widget type
            rhs.type = widget_type_t::mercedes_190e_telltale;
            rhs.config = node["config"].as<Mercedes190ETelltaleConfig_t>();
        }
        else if (type == Mercedes190EClusterGauge::kWidgetName)
        {
            rhs.type = widget_type_t::mercedes_190e_cluster_gauge;
            rhs.config = node["config"].as<Mercedes190EClusterGaugeConfig_t>();
        }
        else if (type == MotecC125Tachometer::kWidgetName)
        {
            rhs.type = widget_type_t::motec_c125_tachometer;
            rhs.config = node["config"].as<MotecC125TachometerConfig_t>();
        }
        else if (type == MotecCdl3Tachometer::kWidgetName)
        {
            rhs.type = widget_type_t::motec_cdl3_tachometer;
            rhs.config = node["config"].as<MotecCdl3TachometerConfig_t>();
        }
        else if (type == StaticTextWidget::kWidgetName)
        {
            rhs.type = widget_type_t::static_text;
            rhs.config = node["config"].as<StaticTextConfig_t>();
        }
        else if (type == ValueReadoutWidget::kWidgetName)
        {
            rhs.type = widget_type_t::value_readout;
            rhs.config = node["config"].as<ValueReadoutConfig_t>();
        }
        else if (type == BackgroundRectWidget::kWidgetName)
        {
            rhs.type = widget_type_t::background_rect;
            rhs.config = node["config"].as<BackgroundRectConfig_t>();
        }
        else
        {
            SPDLOG_WARN("Unknown widget type '{}', unable to parse config.", type);
            rhs.type = widget_type_t::unknown;
        }

        return true;
    }
};

}   // namespace YAML


std::optional<app_config_t> load_app_config(const std::string& config_filepath)
{
    // Default config in case of error.
    std::optional<app_config_t> config = std::nullopt;

    try
    {
        config = YAML::LoadFile(config_filepath).as<app_config_t>();
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