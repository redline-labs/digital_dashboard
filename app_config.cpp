#include "app_config.h"
#include <yaml-cpp/yaml.h>

#include <spdlog/spdlog.h>

#include <string>

// Convert from a YAML Node to a native config_t.
namespace YAML {


template<>
struct convert<carplay_config_t> {
    static Node encode(const carplay_config_t& rhs)
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

    static bool decode(const Node& node, carplay_config_t& rhs)
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
struct convert<speedometer_config_t> {
    static Node encode(const speedometer_config_t& rhs)
    {
        Node node = {};
        node["odometer_value"] = rhs.odometer_value;
        node["max_speed"] = rhs.max_speed;

        return node;
    }

    static bool decode(const Node& node, speedometer_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.odometer_value = node["odometer_value"].as<uint32_t>();
        rhs.max_speed = node["max_speed"].as<uint16_t>();

        return true;
    }
};


template<>
struct convert<tachometer_config_t> {
    static Node encode(const tachometer_config_t& rhs)
    {
        Node node = {};
        node["max_rpm"] = rhs.max_rpm;
        node["redline_rpm"] = rhs.redline_rpm;
        node["show_clock"] = rhs.show_clock;

        return node;
    }

    static bool decode(const Node& node, tachometer_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.max_rpm = node["max_rpm"].as<uint32_t>();
        rhs.redline_rpm = node["redline_rpm"].as<uint16_t>();
        rhs.show_clock = node["show_clock"].as<bool>();

        return true;
    }
};


template<>
struct convert<sparkline_config_t> {
    static Node encode(const sparkline_config_t& rhs)
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

        return node;
    }

    static bool decode(const Node& node, sparkline_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.units = node["units"].as<std::string>();
        rhs.min_value = node["min_value"].as<double>();
        rhs.max_value = node["max_value"].as<double>();
        rhs.line_color = node["line_color"].as<std::string>();
        rhs.text_color = node["text_color"].as<std::string>();
        if (node["font_family"]) rhs.font_family = node["font_family"].as<std::string>();
        if (node["font_size_value"]) rhs.font_size_value = node["font_size_value"].as<uint16_t>();
        if (node["font_size_units"]) rhs.font_size_units = node["font_size_units"].as<uint16_t>();
        rhs.update_rate = node["update_rate"].as<uint16_t>();

        return true;
    }
};


template<>
struct convert<battery_telltale_config_t> {
    static Node encode(const battery_telltale_config_t& rhs)
    {
        Node node = {};
        node["warning_color"] = rhs.warning_color;
        node["normal_color"] = rhs.normal_color;

        return node;
    }

    static bool decode(const Node& node, battery_telltale_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        rhs.warning_color = node["warning_color"].as<std::string>();
        rhs.normal_color = node["normal_color"].as<std::string>();

        return true;
    }
};


template<>
struct convert<widget_config_t> {
    static Node encode(const widget_config_t& rhs)
    {
        Node node = {};
        node["type"] = rhs.type;
        node["x"] = rhs.x;
        node["y"] = rhs.y;
        node["width"] = rhs.width;
        node["height"] = rhs.height;
        if (!rhs.zenoh_key.empty()) {
            node["zenoh_key"] = rhs.zenoh_key;
        }

        if (rhs.type == "carplay")
        {
            node["config"] = std::get<carplay_config_t>(rhs.config);
        }
        else if (rhs.type == "speedometer")
        {
            node["config"] = std::get<speedometer_config_t>(rhs.config);
        }
        else if (rhs.type == "tachometer")
        {
            node["config"] = std::get<tachometer_config_t>(rhs.config);
        }
        else if (rhs.type == "sparkline")
        {
            node["config"] = std::get<sparkline_config_t>(rhs.config);
        }
        /*else if (rhs.type == "battery_telltale")
        {
            node["config"] = std::get<battery_telltale_config_t>(rhs.config);
        }*/
        else
        {
            SPDLOG_WARN("Unknown widget type '{}', unable to parse config.", rhs.type);
        }

        return node;
    }

    static bool decode(const Node& node, widget_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        if (node["type"]) rhs.type = node["type"].as<std::string>();
        if (node["x"]) rhs.x = node["x"].as<uint16_t>();
        if (node["y"]) rhs.y = node["y"].as<uint16_t>();
        if (node["width"]) rhs.width = node["width"].as<uint16_t>();
        if (node["height"]) rhs.height = node["height"].as<uint16_t>();
        if (node["zenoh_key"]) rhs.zenoh_key = node["zenoh_key"].as<std::string>();

        if (rhs.type == "carplay")
        {
            rhs.config = node["config"].as<carplay_config_t>();
        }
        else if (rhs.type == "speedometer")
        {
            rhs.config = node["config"].as<speedometer_config_t>();
        }
        else if (rhs.type == "tachometer")
        {
            rhs.config = node["config"].as<tachometer_config_t>();
        }
        else if (rhs.type == "sparkline")
        {
            rhs.config = node["config"].as<sparkline_config_t>();
        }
        else if (rhs.type == "battery_telltale")
        {
            rhs.config = node["config"].as<battery_telltale_config_t>();
        }
        else
        {
            SPDLOG_WARN("Unknown widget type '{}', unable to parse config.", rhs.type);
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


app_config_t load_app_config(const std::string& config_filepath)
{
    return YAML::LoadFile(config_filepath).as<app_config_t>();
}