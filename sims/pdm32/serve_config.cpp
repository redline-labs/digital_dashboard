#include "serve_config.h"

#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "node_config/reader.h"

namespace pdm32 {

ServeConfig default_serve_config() {
    ServeConfig config;
    config.can.name = "can0";
    config.can.device = "sim:pdm32";
    config.can.rxKey = "vehicle/can0/rx";
    config.can.txKey = "vehicle/can0/tx";
    config.can.recordGeneratedBy = "Redline pdm32_sim";
    return config;
}

std::optional<ServeConfig> parse_serve_config(const std::string& yaml) {
    ServeConfig out = default_serve_config();
    node_config::Context context;

    YAML::Node root;
    try {
        root = YAML::Load(yaml);
    } catch (const YAML::Exception& error) {
        SPDLOG_ERROR("[config] not valid YAML: {}", error.what());
        return std::nullopt;
    }
    if (root.IsNull()) {
        root = YAML::Node(YAML::NodeType::Map);
    }
    if (!root.IsMap()) {
        SPDLOG_ERROR("[config] the file is not a mapping");
        return std::nullopt;
    }

    node_config::rejectUnknownKeys(root, {"can", "scenario", "firmware", "engine", "stepped"}, context, "");
    node_config::readString(root, "scenario", out.scenario, context, "");
    node_config::readString(root, "firmware", out.firmware, context, "");
    node_config::readString(root, "engine", out.engine, context, "");
    node_config::readBool(root, "stepped", out.stepped, context, "");

    if (const YAML::Node can = root["can"]) {
        if (!can.IsMap()) {
            context.fail("can: expected a mapping");
        } else {
            node_config::rejectUnknownKeys(can, {"name", "rx_key", "tx_key", "record_trc"}, context, "can");
            node_config::readString(can, "name", out.can.name, context, "can");
            // Keys follow the name unless given, as can_bridge's do.
            out.can.rxKey = "vehicle/" + out.can.name + "/rx";
            out.can.txKey = "vehicle/" + out.can.name + "/tx";
            node_config::readString(can, "rx_key", out.can.rxKey, context, "can");
            node_config::readString(can, "tx_key", out.can.txKey, context, "can");
            node_config::readString(can, "record_trc", out.can.recordTrcPath, context, "can");
        }
    }

    if (out.can.name.empty()) {
        context.fail("can.name: must not be empty");
    }
    if (out.can.rxKey == out.can.txKey) {
        context.fail("can: rx_key and tx_key are the same topic; the PDM would hear its own frames");
    }
    node_config::checkTopicKey(out.can.rxKey, "can.rx_key", context);
    node_config::checkTopicKey(out.can.txKey, "can.tx_key", context);
    if (!out.engine.empty() && out.engine != "interp" && out.engine != "block" && out.engine != "jit") {
        context.fail("engine: '" + out.engine + "' is not interp, block or jit");
    }

    if (!context.ok) {
        return std::nullopt;
    }
    return out;
}

std::optional<ServeConfig> load_serve_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        SPDLOG_ERROR("[config] cannot read '{}'", path);
        return std::nullopt;
    }
    std::stringstream text;
    text << in.rdbuf();
    return parse_serve_config(text.str());
}

}  // namespace pdm32
