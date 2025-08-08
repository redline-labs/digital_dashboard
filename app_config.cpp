#include "app_config.h"
#include <yaml-cpp/yaml.h>

#include <spdlog/spdlog.h>

#include <string>

// Convert from a YAML Node to a native config_t.
namespace YAML {


template<>
struct convert<CarplayConfig_t> {
    static Node encode(const CarplayConfig_t& rhs)
    {
        Node node = {};

        node["width_px"]            = rhs.width_px;
        node["height_px"]           = rhs.height_px;
        node["fps"]                 = rhs.fps;
        node["dpi"]                 = rhs.dpi;
        node["format"]              = rhs.format;
        node["i_box_version"]       = rhs.i_box_version;
        node["phone_work_mode"]     = rhs.phone_work_mode;
        node["packet_max"]          = rhs.packet_max;
        node["box_name"]            = rhs.box_name;
        node["night_mode"]          = rhs.night_mode;
        //node["drive_type"]          = static_cast<uint32_t>(rhs.drive_type);
        node["media_delay"]         = rhs.media_delay;
        node["audio_transfer_mode"] = rhs.audio_transfer_mode;
        //node["wifi_type"]           = static_cast<uint32_t>(rhs.wifi_type);
        //node["mic_type"]            = static_cast<uint32_t>(rhs.mic_type);

        // TODO: phone config;
        node["audio_device_buffer_size"] = rhs.audio_device_buffer_size;

        return node;
    }

    static bool decode(const Node& node, CarplayConfig_t& rhs)
    {

        rhs.width_px = node["width_px"].as<uint16_t>();
        rhs.height_px = node["height_px"].as<uint16_t>();
        rhs.fps = node["fps"].as<uint8_t>();
        rhs.dpi = node["dpi"].as<uint16_t>();
        rhs.format = node["format"].as<uint8_t>();
        rhs.i_box_version = node["i_box_version"].as<uint8_t>();
        rhs.phone_work_mode = node["phone_work_mode"].as<uint16_t>();
        rhs.packet_max = node["packet_max"].as<uint32_t>();
        rhs.box_name = node["box_name"].as<std::string>();
        rhs.night_mode = node["night_mode"].as<bool>();
        //rhs.drive_type = node["drive_type"].as<uint32_t>();
        rhs.media_delay = node["media_delay"].as<uint16_t>();
        rhs.audio_transfer_mode = node["audio_transfer_mode"].as<bool>();
        //rhs.wifi_type = node["wifi_type"].as<>();
        //rhs.mic_type = node["mic_type"].as<>();

        // TODO: phone config;

        rhs.audio_device_buffer_size = node["audio_device_buffer_size"].as<uint32_t>();

        return true;
    }
};


template<>
struct convert<Mercedes190ESpeedometerConfig_t> {
    static Node encode(const Mercedes190ESpeedometerConfig_t& rhs)
    {
        Node node = {};
        node["odometer_value"] = rhs.odometer_value;
        node["max_speed"] = rhs.max_speed;
        node["zenoh_key"] = rhs.zenoh_key;
        node["shift_box_markers"] = rhs.shift_box_markers;
        node["schema_type"] = rhs.schema_type;
        node["speed_expression"] = rhs.speed_expression;
        node["odometer_expression"] = rhs.odometer_expression;
        node["odometer_zenoh_key"] = rhs.odometer_zenoh_key;
        node["odometer_schema_type"] = rhs.odometer_schema_type;
        return node;
    }

    static bool decode(const Node& node, Mercedes190ESpeedometerConfig_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.odometer_value = node["odometer_value"].as<uint32_t>();
        rhs.max_speed = node["max_speed"].as<uint16_t>();
        rhs.zenoh_key = node["zenoh_key"].as<std::string>();
        rhs.shift_box_markers = node["shift_box_markers"].as<std::vector<uint8_t>>();
        rhs.schema_type = node["schema_type"].as<std::string>();
        rhs.speed_expression = node["speed_expression"].as<std::string>();
        rhs.odometer_expression = node["odometer_expression"].as<std::string>();
        rhs.odometer_zenoh_key = node["odometer_zenoh_key"].as<std::string>();
        rhs.odometer_schema_type = node["odometer_schema_type"].as<std::string>();
        return true;
    }
};


template<>
struct convert<Mercedes190ETachometerConfig_t> {
    static Node encode(const Mercedes190ETachometerConfig_t& rhs)
    {
        Node node = {};
        node["max_rpm"] = rhs.max_rpm;
        node["redline_rpm"] = rhs.redline_rpm;
        node["show_clock"] = rhs.show_clock;
        node["zenoh_key"] = rhs.zenoh_key;
        node["schema_type"] = rhs.schema_type;
        node["rpm_expression"] = rhs.rpm_expression;

        return node;
    }

    static bool decode(const Node& node, Mercedes190ETachometerConfig_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.max_rpm = node["max_rpm"].as<uint32_t>();
        rhs.redline_rpm = node["redline_rpm"].as<uint16_t>();
        rhs.show_clock = node["show_clock"].as<bool>();
        rhs.zenoh_key = node["zenoh_key"].as<std::string>();
        rhs.schema_type = node["schema_type"].as<std::string>();
        rhs.rpm_expression = node["rpm_expression"].as<std::string>();

        return true;
    }
};


template<>
struct convert<SparklineConfig_t> {
    static Node encode(const SparklineConfig_t& rhs)
    {
        Node node = {};
        node["units"] = rhs.units;
        node["min_value"] = rhs.min_value;
        node["max_value"] = rhs.max_value;
        node["line_color"] = rhs.line_color;
        node["text_color"] = rhs.text_color;
        node["font_family"] = rhs.font_family;
        node["font_size_value"] = rhs.font_size_value;
        node["font_size_units"] = rhs.font_size_units;
        node["update_rate"] = rhs.update_rate;
        node["zenoh_key"] = rhs.zenoh_key;
        node["schema_type"] = rhs.schema_type;
        node["value_expression"] = rhs.value_expression;

        return node;
    }

    static bool decode(const Node& node, SparklineConfig_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.units = node["units"].as<std::string>();
        rhs.min_value = node["min_value"].as<double>();
        rhs.max_value = node["max_value"].as<double>();
        rhs.line_color = node["line_color"].as<std::string>();
        rhs.text_color = node["text_color"].as<std::string>();
        rhs.font_family = node["font_family"].as<std::string>();
        rhs.font_size_value = node["font_size_value"].as<uint16_t>();
        rhs.font_size_units = node["font_size_units"].as<uint16_t>();
        rhs.update_rate = node["update_rate"].as<uint16_t>();
        rhs.zenoh_key = node["zenoh_key"].as<std::string>();
        rhs.schema_type = node["schema_type"].as<std::string>();
        rhs.value_expression = node["value_expression"].as<std::string>();

        return true;
    }
};


template<>
struct convert<Mercedes190EBatteryTelltaleConfig_t> {
    static Node encode(const Mercedes190EBatteryTelltaleConfig_t& rhs)
    {
        Node node = {};
        node["warning_color"] = rhs.warning_color;
        node["normal_color"] = rhs.normal_color;
        node["zenoh_key"] = rhs.zenoh_key;
        node["schema_type"] = rhs.schema_type;
        node["condition_expression"] = rhs.condition_expression;
        return node;
    }

    static bool decode(const Node& node, Mercedes190EBatteryTelltaleConfig_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.warning_color = node["warning_color"].as<std::string>();
        rhs.normal_color = node["normal_color"].as<std::string>();
        rhs.zenoh_key = node["zenoh_key"].as<std::string>();
        rhs.schema_type = node["schema_type"].as<std::string>();
        rhs.condition_expression = node["condition_expression"].as<std::string>();
        return true;
    }
};

template<>
struct convert<Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t> {
    static Node encode(const Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& rhs)
    {
        Node node = {};
        node["min_value"] = rhs.min_value;
        node["max_value"] = rhs.max_value;
        node["zenoh_key"] = rhs.zenoh_key;
        node["schema_type"] = rhs.schema_type;
        node["value_expression"] = rhs.value_expression;

        return node;
    }

    static bool decode(const Node& node, Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.min_value = node["min_value"].as<float>();
        rhs.max_value = node["max_value"].as<float>();
        rhs.zenoh_key = node["zenoh_key"].as<std::string>();
        rhs.schema_type = node["schema_type"].as<std::string>();
        rhs.value_expression = node["value_expression"].as<std::string>();
        return true;
    }
};

template<>
struct convert<Mercedes190EClusterGaugeConfig_t> {
    static Node encode(const Mercedes190EClusterGaugeConfig_t& rhs)
    {
        Node node = {};
        node["fuel_gauge"] = rhs.fuel_gauge;
        node["right_gauge"] = rhs.right_gauge;
        node["bottom_gauge"] = rhs.bottom_gauge;
        node["left_gauge"] = rhs.left_gauge;

        return node;
    }

    static bool decode(const Node& node, Mercedes190EClusterGaugeConfig_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.fuel_gauge = node["fuel_gauge"].as<Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t>();
        rhs.right_gauge = node["right_gauge"].as<Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t>();
        rhs.bottom_gauge = node["bottom_gauge"].as<Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t>();
        rhs.left_gauge = node["left_gauge"].as<Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t>();

        return true;
    }
};

template<>
struct convert<widget_config_t> {
    static Node encode(const widget_config_t& rhs)
    {
        Node node = {};

        node["x"] = rhs.x;
        node["y"] = rhs.y;
        node["width"] = rhs.width;
        node["height"] = rhs.height;

        if (rhs.type == widget_type_t::carplay)
        {
            node["type"] = CarPlayWidget::kWidgetName;
            node["config"] = std::get<CarPlayWidget::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::mercedes_190e_speedometer)
        {
            node["type"] = Mercedes190ESpeedometer::kWidgetName;
            node["config"] = std::get<Mercedes190ESpeedometer::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::mercedes_190e_tachometer)
        {
            node["type"] = Mercedes190ETachometer::kWidgetName;
            node["config"] = std::get<Mercedes190ETachometer::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::sparkline)
        {
            node["type"] = SparklineItem::kWidgetName;
            node["config"] = std::get<SparklineItem::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::mercedes_190e_battery_telltale)
        {
            node["type"] = Mercedes190EBatteryTelltale::kWidgetName;
            node["config"] = std::get<Mercedes190EBatteryTelltale::config_t>(rhs.config);
        }
        else if (rhs.type == widget_type_t::mercedes_190e_cluster_gauge)
        {
            node["type"] = Mercedes190EClusterGauge::kWidgetName;
            node["config"] = std::get<Mercedes190EClusterGauge::config_t>(rhs.config);
        }
        else
        {
            SPDLOG_WARN("Unknown widget type '{}', unable to parse config.", widget_type_to_string(rhs.type));
            node["type"] = "unknown";
        }

        return node;
    }

    static bool decode(const Node& node, widget_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        std::string type = node["type"].as<std::string>();
        
        if (node["x"]) rhs.x = node["x"].as<uint16_t>();
        if (node["y"]) rhs.y = node["y"].as<uint16_t>();
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
        else if (type == Mercedes190EBatteryTelltale::kWidgetName)
        {
            rhs.type = widget_type_t::mercedes_190e_battery_telltale;
            rhs.config = node["config"].as<Mercedes190EBatteryTelltaleConfig_t>();
        }
        else if (type == Mercedes190EClusterGauge::kWidgetName)
        {
            rhs.type = widget_type_t::mercedes_190e_cluster_gauge;
            rhs.config = node["config"].as<Mercedes190EClusterGaugeConfig_t>();
        }
        else
        {
            SPDLOG_WARN("Unknown widget type '{}', unable to parse config.", type);
            rhs.type = widget_type_t::unknown;
        }

        return true;
    }
};

template<>
struct convert<window_config_t> {
    static Node encode(const window_config_t& rhs)
    {
        Node node = {};
        node["name"] = rhs.name;
        node["width"] = rhs.width;
        node["height"] = rhs.height;
        node["background_color"] = rhs.background_color;
        node["widgets"] = rhs.widgets;
        return node;
    }

    static bool decode(const Node& node, window_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        if (node["name"]) rhs.name = node["name"].as<std::string>();
        if (node["width"]) rhs.width = node["width"].as<uint16_t>();
        if (node["height"]) rhs.height = node["height"].as<uint16_t>();
        if (node["background_color"]) rhs.background_color = node["background_color"].as<std::string>();
        if (node["widgets"]) rhs.widgets = node["widgets"].as<std::vector<widget_config_t>>();

        return true;
    }
};

template<>
struct convert<app_config_t> {
    static Node encode(const app_config_t& rhs)
    {
        Node node = {};

        node["windows"] = rhs.windows;

        return node;
    }

    static bool decode(const Node& node, app_config_t& rhs)
    {
        // Parse windows configuration - support both single window (legacy) and multiple windows
        if (node["windows"]) {
            rhs.windows = node["windows"].as<std::vector<window_config_t>>();
        } else if (node["window"]) {
            // Legacy single window support - convert to windows array
            window_config_t legacy_window = node["window"].as<window_config_t>();
            if (legacy_window.name.empty()) {
                legacy_window.name = "main";
            }
            rhs.windows.push_back(legacy_window);
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