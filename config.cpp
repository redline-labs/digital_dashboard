#include "config.h"

#include <string>

#include <yaml-cpp/yaml.h>

// Convert from a YAML Node to a native config_t.
namespace YAML {

template<>
struct convert<config_t> {
    static Node encode(const config_t& rhs)
    {
        Node node = {};

        node["width_px"] = rhs.width_px;

        return node;
    }

    static bool decode(const Node& node, config_t& rhs)
    {
        rhs.width_px = node["width_px"].as<uint16_t>();
        return true;
    }
};

}   // namespace yaml


config_t load_config(const std::string& config_filepath)
{
    return YAML::LoadFile(config_filepath).as<config_t>();
}