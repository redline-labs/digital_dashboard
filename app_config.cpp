#include "app_config.h"

#include <string>

#include <yaml-cpp/yaml.h>

// Convert from a YAML Node to a native config_t.
namespace YAML {

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

        return true;
    }
};

template<>
struct convert<window_config_t> {
    static Node encode(const window_config_t& rhs)
    {
        Node node = {};
        node["width"] = rhs.width;
        node["height"] = rhs.height;
        node["widgets"] = rhs.widgets;
        return node;
    }

    static bool decode(const Node& node, window_config_t& rhs)
    {
        if (!node.IsMap()) return false;

        if (node["width"]) rhs.width = node["width"].as<uint16_t>();
        if (node["height"]) rhs.height = node["height"].as<uint16_t>();
        if (node["widgets"]) rhs.widgets = node["widgets"].as<std::vector<widget_config_t>>();

        return true;
    }
};

template<>
struct convert<app_config_t> {
    static Node encode(const app_config_t& rhs)
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
        node["window"] = rhs.window;

        return node;
    }

    static bool decode(const Node& node, app_config_t& rhs)
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

        // Parse window configuration if it exists
        if (node["window"]) {
            rhs.window = node["window"].as<window_config_t>();
        }

        return true;
    }
};

}   // namespace YAML


app_config_t load_app_config(const std::string& config_filepath)
{
    return YAML::LoadFile(config_filepath).as<app_config_t>();
}